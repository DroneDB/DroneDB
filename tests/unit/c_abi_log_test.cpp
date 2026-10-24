/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The host's log callback (new in 2.0): DDBSetLogCallback, DDBSetThreadOperation.
//
//  - libddb's records (plog, source "ddb") and GDAL's messages (source "gdal") reach the
//    callback with the operation id of the thread; a parallel section's workers log with the
//    caller's id, also for GDAL errors raised on them;
//  - the operation id is for the next call: the call resets it when it returns;
//  - replacing or unregistering the callback waits for the threads still inside it;
//  - a record logged while the thread runs the callback is dropped; DDBSetLogCallback and
//    DDBShutdown from inside it are invalid;
//  - DDBShutdown unregisters it;
//  - registered before DDBRegisterProcess, its minLevel sets the process log level, its
//    records replace the console, and it is never called during the process exit (probe).
// Threads meet on latches; no test sleeps nor asserts on a duration.

#include <atomic>
#include <latch>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "c_api_util.h"
#include "ddb.h"
#include "gdal_inc.h"
#include "gtest/gtest.h"
#include "log_forwarding.h"
#include "logger.h"
#include "mio.h"
#include "parallel.h"
#include "process_runner.h"
#include "test.h"

namespace {

using namespace ddb;

#define ASSERT_DDB_OK(call) ASSERT_EQ((call), DDBERR_NONE) << #call << ": " << DDBGetLastError()

struct Record {
    int level;
    std::string message;
    std::string source;
    uint64_t opId;
    std::thread::id thread;
};

// What the callback received
struct Received {
    std::mutex mutex;
    std::vector<Record> records;

    std::vector<Record> matching(const std::string& text) {
        const std::lock_guard lock(mutex);
        std::vector<Record> out;
        for (const auto& r : records)
            if (r.message.find(text) != std::string::npos)
                out.push_back(r);
        return out;
    }
};

void collect(int level, const char* message, const char* source, uint64_t opId, void* userData) {
    auto* received = static_cast<Received*>(userData);
    const std::lock_guard lock(received->mutex);
    received->records.push_back({level, message, source, opId, std::this_thread::get_id()});
}

// Every test starts and ends without a callback and with the library running
class CAbiLog : public ::testing::Test {
   protected:
    void SetUp() override {
        capi::resetShutdownForTests();
        DDBSetLogCallback(nullptr, DDB_LOG_TRACE, nullptr);
    }
    void TearDown() override {
        DDBSetLogCallback(nullptr, DDB_LOG_TRACE, nullptr);
        capi::resetShutdownForTests();
    }
};

TEST_F(CAbiLog, invalidMinLevel) {
    Received received;
    EXPECT_EQ(DDBSetLogCallback(&collect, -1, &received), DDBERR_INVALIDARGS);
    EXPECT_STRNE(DDBGetLastError(), "");
    EXPECT_EQ(DDBSetLogCallback(&collect, DDB_LOG_FATAL + 1, &received), DDBERR_INVALIDARGS);
    EXPECT_EQ(logfwd::callbackMinLevel(), -1);
    // Ignored when unregistering
    ASSERT_DDB_OK(DDBSetLogCallback(nullptr, 42, nullptr));
}

// A record of libddb reaches the callback with its level, "ddb" and the call's operation id,
// which the call resets; records below minLevel are not delivered
TEST_F(CAbiLog, recordsReachTheCallbackWithTheOperationId) {
    Received received;
    ASSERT_DDB_OK(DDBSetLogCallback(&collect, DDB_LOG_WARNING, &received));
    ASSERT_DDB_OK(DDBSetThreadOperation(42));
    ASSERT_DDB_OK(capi::guarded([] {
        LOGI << "log-test info";
        LOGW << "log-test warning";
        LOGE << "log-test error";
    }));
    EXPECT_EQ(logfwd::threadOperation(), 0u) << "the call did not reset the operation id";
    ASSERT_DDB_OK(capi::guarded([] { LOGW << "log-test after"; }));

    EXPECT_TRUE(received.matching("log-test info").empty());
    const auto warning = received.matching("log-test warning");
    ASSERT_EQ(warning.size(), 1u);
    EXPECT_EQ(warning[0].level, DDB_LOG_WARNING);
    EXPECT_EQ(warning[0].source, "ddb");
    EXPECT_EQ(warning[0].opId, 42u);
    const auto error = received.matching("log-test error");
    ASSERT_EQ(error.size(), 1u);
    EXPECT_EQ(error[0].level, DDB_LOG_ERROR);
    const auto after = received.matching("log-test after");
    ASSERT_EQ(after.size(), 1u);
    EXPECT_EQ(after[0].opId, 0u);
}

// The spec's case: a GDAL error raised in a worker of a parallel section reaches the callback
// with the caller's operation id (the worker's thread-local GDAL handler forwards it)
TEST_F(CAbiLog, gdalErrorInAForEachIndexWorkerCarriesTheOperationId) {
    ASSERT_DDB_OK(DDBConfigure(R"({"cpuPoolSize": 4})"));
    struct ResetPool {
        ~ResetPool() { DDBConfigure(R"({"cpuPoolSize": null})"); }
    } resetPool;
    Received received;
    ASSERT_DDB_OK(DDBSetLogCallback(&collect, DDB_LOG_WARNING, &received));
    ASSERT_DDB_OK(DDBSetThreadOperation(7));
    const auto caller = std::this_thread::get_id();
    constexpr size_t kCount = 16;
    ASSERT_DDB_OK(capi::guarded([&] {
        // The first two items run at once, so at least one runs on a worker
        std::latch twoInFlight(2);
        parallel::forEachIndex(kCount, 4, [&](size_t i) {
            if (i < 2)
                twoInFlight.arrive_and_wait();
            CPLError(CE_Warning, CPLE_AppDefined, "log-test gdal %d", static_cast<int>(i));
        });
    }));

    const auto records = received.matching("log-test gdal");
    ASSERT_EQ(records.size(), kCount);
    bool fromWorker = false;
    for (const auto& r : records) {
        EXPECT_EQ(r.level, DDB_LOG_WARNING);
        EXPECT_EQ(r.source, "gdal");
        EXPECT_EQ(r.opId, 7u) << r.message;
        fromWorker = fromWorker || r.thread != caller;
    }
    EXPECT_TRUE(fromWorker) << "no item ran on a worker";
}

// On the calling thread GDAL's process-wide handler forwards too
TEST_F(CAbiLog, gdalErrorOnTheCallingThread) {
    Received received;
    ASSERT_DDB_OK(DDBSetLogCallback(&collect, DDB_LOG_TRACE, &received));
    ASSERT_DDB_OK(DDBSetThreadOperation(9));
    ASSERT_DDB_OK(capi::guarded([] { CPLError(CE_Failure, CPLE_AppDefined, "log-test failure"); }));
    const auto records = received.matching("log-test failure");
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].level, DDB_LOG_ERROR);
    EXPECT_EQ(records[0].source, "gdal");
    EXPECT_EQ(records[0].opId, 9u);
    CPLErrorReset();
}

// Unregistering waits until no thread is inside the callback: afterwards it is never called
struct Blocking {
    std::latch entered{1};
    std::latch release{1};
    std::atomic<bool> left{false};
};

void blockOnce(int, const char* message, const char*, uint64_t, void* userData) {
    auto* state = static_cast<Blocking*>(userData);
    if (std::string(message) != "log-test block")
        return;
    state->entered.count_down();
    state->release.wait();
    state->left = true;
}

TEST_F(CAbiLog, unregisterWaitsForTheThreadsInsideTheCallback) {
    Blocking state;
    ASSERT_DDB_OK(DDBSetLogCallback(&blockOnce, DDB_LOG_TRACE, &state));
    std::thread logger([] { capi::guarded([] { LOGW << "log-test block"; }); });
    state.entered.wait();

    std::atomic<bool> returned{false};
    std::atomic<bool> leftBeforeReturn{false};
    std::thread unregisterer([&] {
        DDBSetLogCallback(nullptr, DDB_LOG_TRACE, nullptr);
        leftBeforeReturn = state.left.load();
        returned = true;
    });
    // Once the callback is swapped out, the unregistering thread can only wait for the logger
    while (logfwd::callbackMinLevel() != -1)
        std::this_thread::yield();
    EXPECT_FALSE(returned) << "DDBSetLogCallback returned while a thread was inside the callback";

    state.release.count_down();
    logger.join();
    unregisterer.join();
    EXPECT_TRUE(returned);
    EXPECT_TRUE(leftBeforeReturn);
}

// Inside the callback: a record logged by the same thread is dropped (no recursion), and the
// calls that would wait for the callback are invalid
struct Reentrant {
    int calls = 0;
    DDBErr setCallback = DDBERR_NONE;
    DDBErr shutdown = DDBERR_NONE;
};

void reenter(int, const char* message, const char*, uint64_t, void* userData) {
    auto* state = static_cast<Reentrant*>(userData);
    if (std::string(message) != "log-test outer")
        return;
    state->calls++;
    LOGW << "log-test inner";  // dropped: this thread runs the callback
    state->setCallback = DDBSetLogCallback(nullptr, DDB_LOG_TRACE, nullptr);
    state->shutdown = DDBShutdown(0);
}

TEST_F(CAbiLog, callbackIsNotReentered) {
    Reentrant state;
    Received received;
    ASSERT_DDB_OK(DDBSetLogCallback(&reenter, DDB_LOG_TRACE, &state));
    // On a thread outside any call, so DDBShutdown is rejected for being in the callback
    std::thread([] { LOGW << "log-test outer"; }).join();
    EXPECT_EQ(state.calls, 1);
    EXPECT_EQ(state.setCallback, DDBERR_INVALIDARGS);
    EXPECT_EQ(state.shutdown, DDBERR_INVALIDARGS);
    EXPECT_FALSE(capi::shutdownStarted());
    EXPECT_EQ(logfwd::callbackMinLevel(), DDB_LOG_TRACE) << "the callback was unregistered from inside";
}

TEST_F(CAbiLog, replacedCallbackIsNotCalledAnyMore) {
    Received first, second;
    ASSERT_DDB_OK(DDBSetLogCallback(&collect, DDB_LOG_TRACE, &first));
    ASSERT_DDB_OK(DDBSetLogCallback(&collect, DDB_LOG_TRACE, &second));
    LOGW << "log-test replaced";
    EXPECT_TRUE(first.matching("log-test replaced").empty());
    EXPECT_EQ(second.matching("log-test replaced").size(), 1u);

    ASSERT_DDB_OK(DDBSetLogCallback(nullptr, DDB_LOG_TRACE, nullptr));
    LOGW << "log-test unregistered";
    EXPECT_TRUE(second.matching("log-test unregistered").empty());
}

TEST_F(CAbiLog, shutdownUnregistersTheCallback) {
    Received received;
    ASSERT_DDB_OK(DDBSetLogCallback(&collect, DDB_LOG_TRACE, &received));
    ASSERT_DDB_OK(DDBShutdown(0));
    EXPECT_EQ(logfwd::callbackMinLevel(), -1);
    LOGW << "log-test after shutdown";
    EXPECT_TRUE(received.matching("log-test after shutdown").empty());
    // Setting it again is refused like any call
    EXPECT_EQ(DDBSetLogCallback(&collect, DDB_LOG_TRACE, &received), DDBERR_SHUTDOWN);
    EXPECT_EQ(DDBSetThreadOperation(1), DDBERR_SHUTDOWN);
}

// Many threads log while the callback is replaced: each record goes to one registration, and
// none reaches a registration after it was replaced (TSan checks the publication)
TEST_F(CAbiLog, concurrentLoggingAndReplacement) {
    Received a, b;
    ASSERT_DDB_OK(DDBSetLogCallback(&collect, DDB_LOG_TRACE, &a));
    constexpr int kThreads = 6;
    std::latch start(kThreads + 1);
    std::atomic<bool> stop{false};
    std::vector<std::thread> loggers;
    for (int i = 0; i < kThreads; i++) {
        loggers.emplace_back([&] {
            start.arrive_and_wait();
            for (int n = 0; n < 5000 && !stop; n++)
                LOGW << "log-test concurrent";
        });
    }
    start.arrive_and_wait();
    for (int i = 0; i < 50; i++) {
        ASSERT_DDB_OK(DDBSetLogCallback(&collect, DDB_LOG_TRACE, i % 2 == 0 ? &b : &a));
    }
    ASSERT_DDB_OK(DDBSetLogCallback(nullptr, DDB_LOG_TRACE, nullptr));
    const size_t aCount = a.matching("log-test concurrent").size();
    const size_t bCount = b.matching("log-test concurrent").size();
    stop = true;
    for (auto& t : loggers)
        t.join();
    // Unregistered before the loggers stopped: nothing more arrived
    EXPECT_EQ(a.matching("log-test concurrent").size(), aCount);
    EXPECT_EQ(b.matching("log-test concurrent").size(), bCount);
}

// Registered before DDBRegisterProcess (its own process: this one registered in main). Started
// with the library's process runner, like every child process of the tests: stdout and stderr
// are captured together.
TEST(CAbiLogProcess, callbackBeforeRegisterProcess) {
    const fs::path probe = io::getExeFolderPath() / DDB_LOG_PROBE_NAME;
    ASSERT_TRUE(fs::exists(probe)) << probe;
    const process::Result r = process::run(probe, {});
    // 3: the callback ran during the exit
    EXPECT_TRUE(r.success()) << r.describe() << "\n" << r.outputTail;
    // The callback replaces the console: the probe prints its own success line and no log record.
    // Other lines are allowed: stderr is captured too, where a sanitizer runtime reports at exit
    // (LSan's "Suppressions used" table in the ASan job).
    // Lines end in "\r\n" on Windows
    std::istringstream lines(r.outputTail);
    const auto nextLine = [&lines](std::string& line) {
        if (!std::getline(lines, line))
            return false;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        return true;
    };
    std::string first;
    nextLine(first);
    EXPECT_EQ(first, "log probe ok") << r.outputTail;
    const std::regex logRecord(R"(^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3} )");
    for (std::string line; nextLine(line);)
        EXPECT_FALSE(std::regex_search(line, logRecord))
            << "the probe printed a log record while its callback was registered: " << line;
}

}  // namespace
