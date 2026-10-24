/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// RetryPolicy::runIf, the predicate form of the policy the Registry client retries with.
// Nothing here sleeps: the policy takes the sleeper and the clock as parameters.

#include <chrono>
#include <stdexcept>
#include <vector>

#include "gtest/gtest.h"
#include "retrypolicy.h"

namespace
{
    using namespace ddb;
    using std::chrono::milliseconds;

    // A clock and a sleeper that move together, as a real wait would
    struct FakeTime
    {
        milliseconds now{0};
        std::vector<milliseconds> sleeps;

        auto sleeper()
        {
            return [this](milliseconds d) {
                sleeps.push_back(d);
                now += d;
            };
        }
        auto clock()
        {
            return [this]() { return now; };
        }
    };

    RetryPolicy policy(unsigned attempts, unsigned deadlineMs = 60000)
    {
        RetryPolicy p;
        p.baseDelayMs = 100;
        p.maxDelayMs = 800;
        p.deadlineMs = deadlineMs;
        p.maxAttempts = attempts;
        return p;
    }

    const auto retryOnNegative = [](int result) {
        return result < 0 ? RetryDecision::again() : RetryDecision::stop();
    };

    TEST(RetryPolicy, ASuccessIsReturnedAtOnceWithoutWaiting)
    {
        FakeTime time;
        int calls = 0;
        const int result = policy(3).runIf([&] { ++calls; return 7; }, retryOnNegative, "op", time.sleeper(), time.clock());
        EXPECT_EQ(result, 7);
        EXPECT_EQ(calls, 1);
        EXPECT_TRUE(time.sleeps.empty());
    }

    TEST(RetryPolicy, RetriesUntilThePredicateIsSatisfied)
    {
        FakeTime time;
        int calls = 0;
        const int result = policy(5).runIf([&] { return ++calls < 3 ? -1 : 42; }, retryOnNegative, "op",
                                           time.sleeper(), time.clock());
        EXPECT_EQ(result, 42);
        EXPECT_EQ(calls, 3);
        EXPECT_EQ(time.sleeps.size(), 2u);
    }

    // The last result is returned, not an exception: the caller maps it to its own error
    TEST(RetryPolicy, TheLastResultIsReturnedWhenTheAttemptsRunOut)
    {
        FakeTime time;
        int calls = 0;
        const int result = policy(3).runIf([&] { return -(++calls); }, retryOnNegative, "op", time.sleeper(), time.clock());
        EXPECT_EQ(calls, 3);
        EXPECT_EQ(result, -3);
        EXPECT_EQ(time.sleeps.size(), 2u); // between the attempts, not after the last
    }

    TEST(RetryPolicy, TheWaitsFollowTheBackoffCapAndNeverExceedIt)
    {
        FakeTime time;
        policy(6).runIf([] { return -1; }, retryOnNegative, "op", time.sleeper(), time.clock());
        ASSERT_EQ(time.sleeps.size(), 5u);
        // Full jitter: in [0, min(maxDelay, base * 2^attempt)]
        const unsigned caps[] = {100, 200, 400, 800, 800};
        for (size_t i = 0; i < time.sleeps.size(); i++)
        {
            EXPECT_GE(time.sleeps[i].count(), 0) << i;
            EXPECT_LE(time.sleeps[i].count(), static_cast<long>(caps[i])) << i;
        }
    }

    TEST(RetryPolicy, AMinimumDelayOfThePredicateIsHonoured)
    {
        FakeTime time;
        int calls = 0;
        policy(3).runIf([&] { return ++calls < 2 ? -1 : 1; },
                        [](int r) { return r < 0 ? RetryDecision::again(milliseconds(5000)) : RetryDecision::stop(); },
                        "op", time.sleeper(), time.clock());
        ASSERT_EQ(time.sleeps.size(), 1u);
        EXPECT_EQ(time.sleeps[0], milliseconds(5000)); // above the backoff cap, so it is what the server asked
    }

    TEST(RetryPolicy, ADelayThatWouldEndAfterTheDeadlineIsNotStarted)
    {
        FakeTime time;
        int calls = 0;
        const int result = policy(0, 3000).runIf([&] { return -(++calls); },
                                                 [](int) { return RetryDecision::again(milliseconds(2000)); },
                                                 "op", time.sleeper(), time.clock());
        // 2 s fits in 3 s, the second wait (4 s in all) does not
        EXPECT_EQ(calls, 2);
        EXPECT_EQ(result, -2);
        EXPECT_EQ(time.sleeps.size(), 1u);
    }

    TEST(RetryPolicy, ARetryAfterBeyondTheDeadlineGivesUpAtOnce)
    {
        FakeTime time;
        int calls = 0;
        policy(5, 10000).runIf([&] { ++calls; return -1; },
                               [](int) { return RetryDecision::again(milliseconds(120000)); },
                               "op", time.sleeper(), time.clock());
        EXPECT_EQ(calls, 1);
        EXPECT_TRUE(time.sleeps.empty());
    }

    // Whether an exception is worth another try is not the predicate's question: it leaves
    TEST(RetryPolicy, AnExceptionOfTheOperationEndsTheCall)
    {
        FakeTime time;
        int calls = 0;
        EXPECT_THROW(policy(3).runIf([&]() -> int { ++calls; throw std::runtime_error("x"); }, retryOnNegative, "op",
                                     time.sleeper(), time.clock()),
                     std::runtime_error);
        EXPECT_EQ(calls, 1);
    }

    TEST(RetryPolicy, ThePoliciesOfTheLibrary)
    {
        const auto network = RetryPolicy::network();
        EXPECT_EQ(network.maxAttempts, 3u);
        EXPECT_EQ(network.deadlineMs, 30000u);
        // The default one (SQLite busy) is unbounded in attempts and keeps its numbers
        const auto &db = RetryPolicy::defaultPolicy();
        EXPECT_EQ(db.maxAttempts, 0u);
        EXPECT_EQ(db.baseDelayMs, 25u);
        EXPECT_EQ(db.maxDelayMs, 2000u);
        EXPECT_EQ(db.deadlineMs, 60000u);
    }
}
