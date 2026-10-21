/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-15 (XI-07): the cancellation checkpoints of the core (cancellation.h): tokens and their
// references, the thread's token and its propagation to forEachIndex workers, GDAL progress
// callbacks (from any thread), the SQLite progress handler and process::run, which kills the
// child. The C API side (DDBCreateCancelToken ...) is in c_abi_cancel_test.cpp.

#include <atomic>
#include <chrono>
#include <latch>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "cancellation.h"
#include "exceptions.h"
#include "gtest/gtest.h"
#include "helpers/process_test_utils.h"
#include "parallel.h"
#include "process_config.h"
#include "process_runner.h"
#include "sqlite_database.h"

namespace {

using namespace ddb;

cancel::TokenRef newToken(cancel::Token* parent = nullptr) {
    return cancel::TokenRef::adopt(cancel::Token::create(parent));
}

TEST(Cancellation, tokenIsCanceledForGoodWithTheFirstMessage) {
    const auto token = newToken();
    EXPECT_FALSE(token.get()->canceled());
    EXPECT_EQ(token.get()->message(), nullptr);
    token.get()->cancel("first");
    token.get()->cancel("second");
    EXPECT_TRUE(token.get()->canceled());
    EXPECT_STREQ(token.get()->message(), "first");
}

TEST(Cancellation, childIsCanceledWithItsParent) {
    const auto parent = newToken();
    const auto child = newToken(parent.get());
    const auto other = newToken(parent.get());
    child.get()->cancel("child only");
    EXPECT_TRUE(child.get()->canceled());
    EXPECT_FALSE(parent.get()->canceled());
    EXPECT_FALSE(other.get()->canceled());

    parent.get()->cancel(cancel::kCanceledByToken);
    EXPECT_STREQ(other.get()->message(), cancel::kCanceledByToken);
    EXPECT_STREQ(child.get()->message(), "child only");
}

// The child holds a reference on its parent: the parent outlives the caller's reference
TEST(Cancellation, childKeepsItsParentAlive) {
    cancel::Token* parent = cancel::Token::create();
    const auto child = newToken(parent);
    parent->cancel("parent");
    parent->release();  // the caller's reference: the child's one is left
    EXPECT_STREQ(child.get()->message(), "parent");
}

TEST(Cancellation, scopedThreadTokenPutsThePreviousOneBack) {
    EXPECT_FALSE(cancel::hasThreadToken());
    const auto outer = newToken();
    const auto inner = newToken();
    {
        const cancel::ScopedThreadToken a(outer);
        EXPECT_EQ(cancel::threadToken().get(), outer.get());
        {
            const cancel::ScopedThreadToken b(inner);
            EXPECT_EQ(cancel::threadToken().get(), inner.get());
        }
        EXPECT_EQ(cancel::threadToken().get(), outer.get());
    }
    EXPECT_FALSE(cancel::hasThreadToken());
}

TEST(Cancellation, throwIfCanceledThrowsTheTokensMessage) {
    EXPECT_NO_THROW(cancel::throwIfCanceled());
    const auto token = newToken();
    const cancel::ScopedThreadToken scope(token);
    EXPECT_NO_THROW(cancel::throwIfCanceled());
    token.get()->cancel(cancel::kCanceledByToken);
    try {
        cancel::throwIfCanceled();
        FAIL() << "not thrown";
    } catch (const ShutdownException&) {
        FAIL() << "a canceled token is not the shutdown";
    } catch (const CanceledException& e) {
        EXPECT_STREQ(e.what(), cancel::kCanceledByToken);
    }
}

// A call token: canceled by itself (a progress callback) or by the caller's token, the thread's
// token while in scope
TEST(Cancellation, callTokenIsAChildMadeTheThreadsToken) {
    const auto caller = newToken();
    const cancel::ScopedThreadToken scope(caller);
    {
        cancel::CallToken call;
        EXPECT_NE(cancel::threadToken().get(), caller.get());
        EXPECT_FALSE(cancel::isCanceled());
        caller.get()->cancel("by the caller");
        EXPECT_TRUE(cancel::isCanceled());
    }
    EXPECT_EQ(cancel::threadToken().get(), caller.get());
}

// The workers of a parallel section work for the caller's token, and a canceled token stops
// the loop: no new index starts after it
TEST(Cancellation, forEachIndexWorkersUseTheCallersTokenAndStop) {
    const auto token = newToken();
    const cancel::ScopedThreadToken scope(token);
    constexpr size_t kCount = 256;
    std::atomic<size_t> ran{0};
    std::atomic<size_t> otherToken{0};
    std::atomic<size_t> after{0};
    std::atomic<bool> canceled{false};
    EXPECT_THROW(parallel::forEachIndexIo(kCount,
                                          [&](size_t i) {
                                              ran++;
                                              if (cancel::threadToken().get() != token.get())
                                                  otherToken++;
                                              if (canceled)
                                                  after++;
                                              if (i == 8) {
                                                  token.get()->cancel(cancel::kCanceledByToken);
                                                  canceled = true;
                                              }
                                          }),
                 CanceledException);
    EXPECT_EQ(otherToken.load(), 0u);
    EXPECT_LT(ran.load(), kCount);
    // Only the items that had passed their check when the token was canceled may still run:
    // at most one per thread
    EXPECT_LE(after.load(), static_cast<size_t>(ProcessConfig::get().ioThreads()));
}

// GDAL may call a progress callback from its own threads: the token is the one of the thread
// that created the GdalProgress
TEST(Cancellation, gdalProgressStopsFromAnyThread) {
    const auto token = newToken();
    std::optional<cancel::GdalProgress> progress;
    {
        const cancel::ScopedThreadToken scope(token);
        progress.emplace();
    }
    int beforeCancel = -1, afterCancel = -1;
    std::thread gdalThread([&] {
        beforeCancel = cancel::GdalProgress::callback(0.1, "", progress->data());
        token.get()->cancel(cancel::kCanceledByToken);
        afterCancel = cancel::GdalProgress::callback(0.2, "", progress->data());
    });
    gdalThread.join();
    EXPECT_EQ(beforeCancel, TRUE);
    EXPECT_EQ(afterCancel, FALSE);
}

TEST(Cancellation, gdalProgressCallsItsObserverFirst) {
    std::vector<double> seen;
    const auto token = newToken();
    const cancel::ScopedThreadToken scope(token);
    cancel::GdalProgress progress([&](double fraction) {
        seen.push_back(fraction);
        if (fraction >= 0.5)
            token.get()->cancel("by the observer");
    });
    EXPECT_EQ(cancel::GdalProgress::callback(0.25, "", progress.data()), TRUE);
    EXPECT_EQ(cancel::GdalProgress::callback(0.5, "", progress.data()), FALSE);
    EXPECT_EQ(seen, (std::vector<double>{0.25, 0.5}));
}

// The progress handler reads the running thread's token: a statement of a canceled thread is
// interrupted, the same connection then works for a thread without a canceled token
TEST(Cancellation, sqliteStatementIsInterruptedAndTheConnectionStaysUsable) {
    SqliteDatabase db;
    db.open(":memory:");
    const char* longQuery =
        "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE x < 2000000000) "
        "SELECT count(*) FROM c";
    {
        const auto token = newToken();
        const cancel::ScopedThreadToken scope(token);
        token.get()->cancel(cancel::kCanceledByToken);
        auto q = db.query(longQuery);
        EXPECT_THROW(q->fetch(), CanceledException);
    }
    auto q = db.query("WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE x < 5000) "
                      "SELECT count(*) FROM c");
    ASSERT_TRUE(q->fetch());
    EXPECT_EQ(q->getInt(0), 5000);
}

// Not while an exception unwinds: the cleanup of a canceled call (a rollback in a destructor)
// runs its statements whole
TEST(Cancellation, sqliteStatementsRunWhileACancellationUnwinds) {
    SqliteDatabase db;
    db.open(":memory:");
    const auto token = newToken();
    const cancel::ScopedThreadToken scope(token);
    token.get()->cancel(cancel::kCanceledByToken);
    int cleanupCount = 0;
    try {
        struct CleanupOnUnwind {
            SqliteDatabase& db;
            int& count;
            ~CleanupOnUnwind() {
                // Many more instructions than the progress handler's period
                auto q = db.query("WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE x < 200000) "
                                  "SELECT count(*) FROM c");
                if (q->fetch())
                    count = q->getInt(0);
            }
        } cleanup{db, cleanupCount};
        cancel::throwIfCanceled();
    } catch (const CanceledException&) {
    }
    EXPECT_EQ(cleanupCount, 200000);
}

// process::run kills the child when the work is canceled, from the main supervise loop (a
// child that keeps writing) and after the child closed its pipes, then throws
TEST(Cancellation, processRunKillsTheChildAndThrows) {
    for (const std::vector<std::string>& args :
         {std::vector<std::string>{"descendant-worker", "streaming"}, std::vector<std::string>{"close-output", "30000", "0"}}) {
        SCOPED_TRACE(args.front());
        const auto token = newToken();
        const cancel::ScopedThreadToken scope(token);
        process::Options opts;
        opts.onOutputLine = [&](const std::string&) { token.get()->cancel(cancel::kCanceledByToken); };
        const auto start = std::chrono::steady_clock::now();
        EXPECT_THROW(process::run(testChildPath(), args, opts), CanceledException);
        // Generous: the child would run for 30 s (or forever)
        EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(10));
    }
}

}  // namespace
