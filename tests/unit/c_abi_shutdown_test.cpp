/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-15 (XI-07): DDBShutdown (ABI 1.1).
//
//  - once DDBShutdown started, every call but the exempt accessors returns DDBERR_SHUTDOWN
//    without running; the exempt accessors keep working and keep the last error;
//  - DDBShutdown waits for the calls in flight (counted by the mapper, never itself), then
//    checkpoints (TRUNCATE) and closes the pooled connections and the raster cache, once;
//  - on timeout it returns DDBERR_BUSY and leaves the teardown to a later DDBShutdown;
//  - called from inside a guarded call it returns DDBERR_INVALIDARGS and changes nothing.
//
// The shutdown is process-wide: every test puts the library back in the running state
// (capi::resetShutdownForTests) so the other tests of this binary are not affected. Threads
// meet on latches; a test never sleeps nor asserts on a duration.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <latch>
#include <string>
#include <thread>
#include <vector>

#include "c_api_util.h"
#include "databasepool.h"
#include "ddb.h"
#include "exceptions.h"
#include "gtest/gtest.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace {

using namespace ddb;

#define ASSERT_DDB_OK(call) ASSERT_EQ((call), DDBERR_NONE) << #call << ": " << DDBGetLastError()

char* const kUntouched = reinterpret_cast<char*>(static_cast<uintptr_t>(0x1));

// Every test starts and ends with the library running
class Shutdown : public ::testing::Test {
   protected:
    void SetUp() override { capi::resetShutdownForTests(); }
    void TearDown() override { capi::resetShutdownForTests(); }
};

// Counts the teardowns capi::shutdown runs (the tests that do not need the real one)
std::atomic<int> teardowns{0};
void countTeardown() {
    teardowns++;
}

// Waits (yielding) until DDBShutdown on another thread has started: a condition, not a time
void awaitShutdownStarted() {
    while (!capi::shutdownStarted())
        std::this_thread::yield();
}

// A guarded call held in flight on its own thread until released
class CallInFlight {
   public:
    CallInFlight() {
        thread_ = std::thread([this] {
            result_ = capi::guarded([this] {
                entered_.count_down();
                release_.wait();
                finished_ = true;
            });
        });
        entered_.wait();
    }
    ~CallInFlight() {
        release();
        if (thread_.joinable())
            thread_.join();
    }
    void release() { release_.count_down(); }
    DDBErr join() {
        thread_.join();
        return result_;
    }
    bool finished() const { return finished_; }

   private:
    std::latch entered_{1};
    std::latch release_{1};
    std::atomic<bool> finished_{false};
    DDBErr result_ = DDBERR_EXCEPTION;
    std::thread thread_;
};

TEST_F(Shutdown, refusesEveryCallButTheExemptAccessors) {
    ASSERT_DDB_OK(DDBShutdown(0));
    EXPECT_STREQ(DDBGetLastError(), "");

    // Refused without running: SHUTDOWN with a message, the output untouched
    char* out = kUntouched;
    EXPECT_EQ(DDBNormalizePath("a/b", nullptr, &out), DDBERR_SHUTDOWN);
    EXPECT_EQ(out, kUntouched);
    const std::string message = DDBGetLastError();
    EXPECT_NE(message.find("shut down"), std::string::npos) << message;
    EXPECT_EQ(DDBConfigure("{}"), DDBERR_SHUTDOWN);
    EXPECT_EQ(DDBRegisterProcess(false), DDBERR_SHUTDOWN);
    EXPECT_EQ(DDBGetEffectiveConfig(&out), DDBERR_SHUTDOWN);
    EXPECT_EQ(out, kUntouched);
    EXPECT_EQ(DDBGetLastError(), message);

    // The exempt accessors work and keep the last error
    EXPECT_NE(DDBGetVersion(), nullptr);
    EXPECT_NE(DDBGetAbiVersion(), 0u);
    EXPECT_NE(DDBGetCapabilities(), nullptr);
    EXPECT_EQ(DDBFree(nullptr), DDBERR_NONE);
    EXPECT_EQ(DDBVSIFree(nullptr), DDBERR_NONE);
    EXPECT_STREQ(DDBGetLastErrorReason(), "");
    EXPECT_EQ(DDBGetLastError(), message);

    // Idempotent: shut down already, and it clears the last error like any call
    ASSERT_DDB_OK(DDBShutdown(0));
    EXPECT_STREQ(DDBGetLastError(), "");
    ASSERT_DDB_OK(DDBShutdown(-1));
}

TEST_F(Shutdown, waitsForTheCallsInFlightThenTearsDownOnce) {
    teardowns = 0;
    CallInFlight call;
    ASSERT_EQ(capi::callsInFlight(), 1);

    std::atomic<bool> callFinishedAtTeardown{false};
    static std::atomic<bool>* seen = nullptr;
    static CallInFlight* inFlight = nullptr;
    seen = &callFinishedAtTeardown;
    inFlight = &call;
    DDBErr shutdownResult = DDBERR_EXCEPTION;
    std::thread stopper([&] {
        shutdownResult = capi::shutdown(-1, [] {
            countTeardown();
            *seen = inFlight->finished() && capi::callsInFlight() == 0;
        });
    });
    awaitShutdownStarted();

    // Shutting down: a new call is refused, the one in flight keeps running
    char* out = kUntouched;
    EXPECT_EQ(DDBNormalizePath("a", nullptr, &out), DDBERR_SHUTDOWN);
    EXPECT_EQ(teardowns.load(), 0);
    EXPECT_FALSE(call.finished());

    call.release();
    EXPECT_EQ(call.join(), DDBERR_NONE);  // it ran to its end
    stopper.join();
    EXPECT_EQ(shutdownResult, DDBERR_NONE) << DDBGetLastError();
    EXPECT_EQ(teardowns.load(), 1);
    EXPECT_TRUE(callFinishedAtTeardown) << "the teardown ran while a call was in flight";

    // Once
    ASSERT_DDB_OK(capi::shutdown(0, &countTeardown));
    EXPECT_EQ(teardowns.load(), 1);
}

TEST_F(Shutdown, timeoutReturnsBusyAndLeavesTheTeardownToALaterCall) {
    teardowns = 0;
    CallInFlight call;

    EXPECT_EQ(capi::shutdown(0, &countTeardown), DDBERR_BUSY);
    const std::string message = DDBGetLastError();
    EXPECT_NE(message.find("1 call still running"), std::string::npos) << message;
    EXPECT_EQ(teardowns.load(), 0);
    EXPECT_TRUE(capi::shutdownStarted());

    // New calls are refused already
    char* out = kUntouched;
    EXPECT_EQ(DDBNormalizePath("a", nullptr, &out), DDBERR_SHUTDOWN);

    // Still busy while the call runs
    EXPECT_EQ(capi::shutdown(0, &countTeardown), DDBERR_BUSY);
    EXPECT_EQ(teardowns.load(), 0);

    call.release();
    EXPECT_EQ(call.join(), DDBERR_NONE);
    ASSERT_DDB_OK(capi::shutdown(0, &countTeardown));
    EXPECT_EQ(teardowns.load(), 1);
}

// A call nested in a guarded call (a progress callback) would wait for itself
TEST_F(Shutdown, fromInsideAGuardedCallIsInvalidArgs) {
    DDBErr nested = DDBERR_NONE;
    std::string nestedMessage;
    ASSERT_DDB_OK(capi::guarded([&] {
        nested = DDBShutdown(0);
        nestedMessage = DDBGetLastError();
    }));
    EXPECT_EQ(nested, DDBERR_INVALIDARGS);
    EXPECT_NE(nestedMessage.find("inside a libddb call"), std::string::npos) << nestedMessage;
    EXPECT_FALSE(capi::shutdownStarted());
    EXPECT_EQ(capi::callsInFlight(), 0);
}

// The progress callback of a real export
int shutdownFromProgress(double, const char*, void* userData) {
    *static_cast<DDBErr*>(userData) = DDBShutdown(0);
    return 1;  // and cancel the export
}

TEST_F(Shutdown, fromAProgressCallbackIsInvalidArgs) {
    TestArea area(TEST_NAME, true);
    const auto input = area.getPath("in.tif");
    testutils::GeoRasterSpec spec;
    spec.width = 64;
    spec.height = 64;
    testutils::createGeoRaster(input, spec);
    DDBErr nested = DDBERR_NONE;
    EXPECT_EQ(DDBExportRaster2(input.string().c_str(), area.getPath("out.tif").string().c_str(), nullptr,
                               nullptr, nullptr, nullptr, nullptr, nullptr, 0, &shutdownFromProgress, &nested),
              DDBERR_CANCELED);
    EXPECT_EQ(nested, DDBERR_INVALIDARGS);
    EXPECT_FALSE(capi::shutdownStarted());
}

// A call in flight that stops because of the shutdown reports SHUTDOWN, also from an ABI 1.0
// export (kFrozenCodes)
TEST_F(Shutdown, aCallStoppedByTheShutdownReportsShutdown) {
    EXPECT_EQ(capi::guarded([] { throw ShutdownException("stopped: libddb is shutting down"); }), DDBERR_SHUTDOWN);
    EXPECT_STREQ(DDBGetLastError(), "stopped: libddb is shutting down");
}

// Many threads call while one shuts down: each call runs whole or is refused, and after
// DDBShutdown returned none is in flight and every call is refused
TEST_F(Shutdown, concurrentCallsAndShutdown) {
    constexpr int kThreads = 8;
    std::latch start(kThreads + 1);
    std::atomic<bool> stop{false};
    std::atomic<int> unexpected{0};
    std::vector<std::thread> callers;
    for (int i = 0; i < kThreads; i++) {
        callers.emplace_back([&] {
            start.arrive_and_wait();
            while (!stop) {
                char* out = nullptr;
                const DDBErr rc = DDBNormalizePath("a/./b", nullptr, &out);
                if (rc == DDBERR_NONE)
                    DDBFree(out);
                else if (rc != DDBERR_SHUTDOWN)
                    unexpected++;
            }
        });
    }
    start.arrive_and_wait();
    teardowns = 0;
    const DDBErr rc = capi::shutdown(-1, &countTeardown);
    EXPECT_EQ(capi::callsInFlight(), 0);
    char* out = kUntouched;
    EXPECT_EQ(DDBNormalizePath("a", nullptr, &out), DDBERR_SHUTDOWN);
    stop = true;
    for (auto& t : callers)
        t.join();
    EXPECT_EQ(rc, DDBERR_NONE) << DDBGetLastError();
    EXPECT_EQ(teardowns.load(), 1);
    EXPECT_EQ(unexpected.load(), 0);
}

// The real teardown: the WAL of the pooled connections is moved into the database file
// (TRUNCATE: an empty -wal file) and the connections are closed
TEST_F(Shutdown, checkpointsAndClosesThePooledConnections) {
    const auto root = makeIndex(TEST_NAME);
    const auto rootPath = root.string();
    ASSERT_DDB_OK(DDBSetTag(rootPath.c_str(), "org/ds"));  // a write: frames in the WAL
    ASSERT_GT(DatabasePool::instance().stats().idle, 0u) << "the connection is not pooled";
    const auto wal = root / DDB_FOLDER / "dbase.sqlite-wal";
    ASSERT_TRUE(std::filesystem::exists(wal));
    ASSERT_GT(std::filesystem::file_size(wal), 0u);

    ASSERT_DDB_OK(DDBShutdown(10000));
    EXPECT_EQ(DatabasePool::instance().stats().idle, 0u);
    EXPECT_EQ(std::filesystem::file_size(wal), 0u);
}

// Without the test reset: the process exits normally after DDBShutdown (the exit-time
// checkpoint finds an empty pool)
TEST(ShutdownProcess, exitsNormallyAfterShutdown) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            const auto root = makeIndex("ShutdownProcess-exit");
            const auto rootPath = root.string();
            if (DDBSetTag(rootPath.c_str(), "org/ds") != DDBERR_NONE || DDBShutdown(10000) != DDBERR_NONE)
                std::exit(1);
            char* out = nullptr;
            if (DDBGetTag(rootPath.c_str(), &out) != DDBERR_SHUTDOWN)
                std::exit(2);
            std::fprintf(stderr, "shut down\n");
            std::exit(0);
        },
        ::testing::ExitedWithCode(0), "shut down");
}

}  // namespace
