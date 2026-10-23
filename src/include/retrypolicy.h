/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef RETRYPOLICY_H
#define RETRYPOLICY_H

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>

#include "ddb_export.h"
#include "exceptions.h"
#include "logger.h"
#include "randomutil.h"

namespace ddb {

// What a predicate answers after an attempt of RetryPolicy::runIf: try again (not before
// `minDelay`, e.g. the Retry-After a server sent), or return the result as it is.
struct RetryDecision {
    bool retry = false;
    std::chrono::milliseconds minDelay{0};

    static RetryDecision stop() { return {}; }
    static RetryDecision again(std::chrono::milliseconds minDelay = std::chrono::milliseconds{0}) {
        return {true, minDelay};
    }
};

// The real clock and sleep of the policy. runIf takes them as parameters so that a test runs a
// whole retry schedule without waiting.
struct SteadyRetryClock {
    std::chrono::milliseconds operator()() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch());
    }
};
struct ThreadRetrySleeper {
    void operator()(std::chrono::milliseconds delay) const { std::this_thread::sleep_for(delay); }
};

// Bounded exponential backoff with full jitter. Two ways to use it:
//  * run(): retries an operation that fails with SQLITE_BUSY/SQLITE_LOCKED (surfaced as
//    DBBusyException);
//  * runIf(): retries an operation whose result a predicate judges (an HTTP response that is a
//    transport error, a 5xx or a 429).
// Full jitter (rand(0, backoff), not a fixed or "equal jitter" schedule) is what de-synchronises a
// herd of writers contending for the same lock, or of clients retrying the same server.
struct DDB_DLL RetryPolicy {
    unsigned baseDelayMs = 25;
    unsigned maxDelayMs = 2000;
    unsigned deadlineMs = 60000;
    unsigned maxAttempts = 0; // runIf: attempts in all, the first included; 0 = until the deadline

    static const RetryPolicy &defaultPolicy();

    // The policy of the requests to a Registry: 3 attempts within 30 s (0.5 s base, 4 s cap)
    static RetryPolicy network() {
        RetryPolicy policy;
        policy.baseDelayMs = 500;
        policy.maxDelayMs = 4000;
        policy.deadlineMs = 30000;
        policy.maxAttempts = 3;
        return policy;
    }

    // The wait before the retry that follows attempt number `attempt` (0 for the first): a random
    // time in [0, min(maxDelayMs, baseDelayMs * 2^attempt)]. A per-thread engine (randomutil.h):
    // std::rand() would race between the contending callers this jitter is meant to desynchronize.
    unsigned backoffMs(unsigned attempt) const {
        const unsigned backoff = std::min(maxDelayMs, baseDelayMs << std::min(attempt, 16u));
        return backoff == 0 ? 0 : randomInt<unsigned>(0, backoff);
    }

    // Runs `op` until it succeeds or the retry budget (deadlineMs) is exhausted, retrying only on
    // DBBusyException. `what` is used for the exception message on final failure/log context.
    template <typename Op>
    void run(Op &&op, const char *what) const {
        const auto start = std::chrono::steady_clock::now();
        unsigned attempt = 0;

        for (;;) {
            try {
                op();
                return;
            } catch (const DBBusyException &e) {
                const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - start)
                                           .count();
                if (static_cast<unsigned long long>(elapsedMs) >= deadlineMs)
                    throw DBBusyException(std::string("Retry deadline exceeded for ") + what +
                                          ": " + e.what());

                const unsigned delay = backoffMs(attempt);
                LOGD << "Retrying " << what << " after " << delay << "ms (attempt " << attempt << "): " << e.what();
                if (delay > 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                ++attempt;
            }
        }
    }

    // Calls `op` and returns its result, after trying again while `retry(result)` says so. The
    // result of the last attempt is returned (never an exception of the policy) when the attempts
    // (maxAttempts) or the time (deadlineMs: a wait that would end after it is not started) run
    // out: the caller maps a failed result to its own error. An exception of `op` ends the call:
    // whether it is worth another try is the predicate's question, asked of results only.
    // The waits go through `sleep(std::chrono::milliseconds)` and the time through
    // `now() -> std::chrono::milliseconds`.
    template <typename Op, typename Predicate, typename Sleeper = ThreadRetrySleeper,
              typename Clock = SteadyRetryClock>
    auto runIf(Op &&op, Predicate &&retry, const char *what, Sleeper &&sleep = Sleeper{},
               Clock &&now = Clock{}) const -> decltype(op()) {
        const auto start = now();

        for (unsigned attempt = 0;; ++attempt) {
            auto result = op();

            const RetryDecision decision = retry(result);
            if (!decision.retry)
                return result;
            if (maxAttempts != 0 && attempt + 1 >= maxAttempts)
                return result;

            const auto delay = std::max(std::chrono::milliseconds(backoffMs(attempt)), decision.minDelay);
            if ((now() - start) + delay > std::chrono::milliseconds(deadlineMs))
                return result;

            LOGD << "Retrying " << what << " after " << delay.count() << "ms (attempt " << attempt + 1 << ")";
            sleep(delay);
        }
    }
};

// Retries a single write statement on DBBusyException using the default policy. For stand-alone
// single-statement writers that also run while another Transaction is open on the connection (a
// nested BEGIN would fail): mirrors Transaction's BEGIN/COMMIT retry without opening a
// transaction. busy_timeout is only a 200 ms floor; RetryPolicy owns the wait budget.
template <typename Exec>
void executeWithRetry(Exec &&exec, const char *what) {
    RetryPolicy::defaultPolicy().run(std::forward<Exec>(exec), what);
}

} // namespace ddb

#endif // RETRYPOLICY_H
