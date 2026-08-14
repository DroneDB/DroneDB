/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef RETRYPOLICY_H
#define RETRYPOLICY_H

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>

#include "ddb_export.h"
#include "exceptions.h"
#include "logger.h"

namespace ddb {

// Bounded exponential backoff with full jitter, used to retry operations that fail with
// SQLITE_BUSY/SQLITE_LOCKED (surfaced as DBBusyException). Full jitter (rand(0, backoff), not a
// fixed or "equal jitter" schedule) is what de-synchronises a herd of writers contending for the
// same lock.
struct DDB_DLL RetryPolicy {
    unsigned baseDelayMs = 25;
    unsigned maxDelayMs = 2000;
    unsigned deadlineMs = 60000;

    static const RetryPolicy &defaultPolicy();

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

                const unsigned backoff = std::min(maxDelayMs, baseDelayMs << std::min(attempt, 16u));
                // thread_local RNG: std::rand() shares global state across threads and is not
                // safe to call concurrently, which would both race and de-correlate the jitter
                // this backoff relies on to desynchronize contending writers.
                thread_local std::mt19937 rng{std::random_device{}()};
                const unsigned delay = backoff == 0 ? 0
                    : std::uniform_int_distribution<unsigned>(0, backoff)(rng);
                LOGD << "Retrying " << what << " after " << delay << "ms (attempt " << attempt << "): " << e.what();
                if (delay > 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                ++attempt;
            }
        }
    }
};

} // namespace ddb

#endif // RETRYPOLICY_H
