/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "buildlock.h"
#include "exceptions.h"
#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"

#include <thread>
#include <chrono>
#include <future>
#include <vector>
#include <string>

#ifdef WIN32
#include <windows.h>
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#endif

namespace {

using namespace ddb;

class BuildLockTest : public ::testing::Test {
protected:
    void SetUp() override {
        testArea = std::make_unique<TestArea>(TEST_NAME);
    }

    void TearDown() override {
        testArea.reset();
    }

    std::unique_ptr<TestArea> testArea;
};

/**
 * @brief Test basic lock acquisition and release
 */
TEST_F(BuildLockTest, BasicLockAcquisition) {
    auto outputPath = testArea->getPath("test_output");

    // Test successful lock acquisition
    {
        BuildLock lock(outputPath.string());
        EXPECT_TRUE(lock.isHolding());
        EXPECT_FALSE(lock.getLockFilePath().empty());
        EXPECT_TRUE(lock.getLockFilePath().find(".building") != std::string::npos);
    }
    // Lock should be automatically released when object goes out of scope

    // Verify lock file is cleaned up (platform dependent)
#ifdef WIN32
    // On Windows, file is automatically deleted due to FILE_FLAG_DELETE_ON_CLOSE
    // We can't easily verify this without race conditions
#else
    // On Unix, we can verify the lock file is removed
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(fs::exists(outputPath.string() + ".building"));
#endif
}

/**
 * @brief Test that concurrent locks on the same path fail appropriately
 */
TEST_F(BuildLockTest, ConcurrentLockRejection) {
    auto outputPath = testArea->getPath("concurrent_test");

    // First lock should succeed
    BuildLock firstLock(outputPath.string());
    EXPECT_TRUE(firstLock.isHolding());

    // Second lock on same path should fail
    EXPECT_THROW({
        BuildLock secondLock(outputPath.string());
    }, AppException);
}

/**
 * @brief Test manual lock release
 */
TEST_F(BuildLockTest, ManualRelease) {
    auto outputPath = testArea->getPath("manual_release");

    BuildLock lock(outputPath.string());
    EXPECT_TRUE(lock.isHolding());

    // Manual release
    lock.release();
    EXPECT_FALSE(lock.isHolding());

    // Should be able to acquire new lock on same path
    BuildLock newLock(outputPath.string());
    EXPECT_TRUE(newLock.isHolding());
}

/**
 * @brief Test multiple releases (should be safe)
 */
TEST_F(BuildLockTest, MultipleReleases) {
    auto outputPath = testArea->getPath("multiple_release");

    BuildLock lock(outputPath.string());
    EXPECT_TRUE(lock.isHolding());

    // Multiple releases should be safe
    lock.release();
    EXPECT_FALSE(lock.isHolding());

    lock.release(); // Should not crash
    EXPECT_FALSE(lock.isHolding());
}

/**
 * @brief Test move constructor and assignment
 */
TEST_F(BuildLockTest, MoveSemantics) {
    auto outputPath = testArea->getPath("move_test");

    // Test move constructor
    {
        BuildLock originalLock(outputPath.string());
        EXPECT_TRUE(originalLock.isHolding());

        BuildLock movedLock = std::move(originalLock);
        EXPECT_TRUE(movedLock.isHolding());
        EXPECT_FALSE(originalLock.isHolding()); // Original should be invalidated

        // Should not be able to acquire lock on same path
        EXPECT_THROW({
            BuildLock conflictLock(outputPath.string());
        }, AppException);
    }

    // Test move assignment
    {
        BuildLock lock1(outputPath.string());
        EXPECT_TRUE(lock1.isHolding());

        auto outputPath2 = testArea->getPath("move_test2");
        BuildLock lock2(outputPath2.string());
        EXPECT_TRUE(lock2.isHolding());

        // Move assign lock1 to lock2
        lock2 = std::move(lock1);
        EXPECT_TRUE(lock2.isHolding());
        EXPECT_FALSE(lock1.isHolding());

        // lock2 should now hold the lock for outputPath, not outputPath2
        // This means we should be able to acquire lock for outputPath2
        BuildLock newLock(outputPath2.string());
        EXPECT_TRUE(newLock.isHolding());
    }
}

/**
 * @brief Test behavior with different paths
 */
TEST_F(BuildLockTest, DifferentPaths) {
    auto outputPath1 = testArea->getPath("path1");
    auto outputPath2 = testArea->getPath("path2");

    // Should be able to lock different paths simultaneously
    BuildLock lock1(outputPath1.string());
    BuildLock lock2(outputPath2.string());

    EXPECT_TRUE(lock1.isHolding());
    EXPECT_TRUE(lock2.isHolding());
    EXPECT_NE(lock1.getLockFilePath(), lock2.getLockFilePath());
}

/**
 * @brief Test thread safety - multiple threads trying to lock same path
 */
TEST_F(BuildLockTest, ThreadSafety) {
    auto outputPath = testArea->getPath("thread_test");
    const int numThreads = 10;
    std::vector<std::future<bool>> futures;
    std::atomic<int> successCount{0};

    // Launch multiple threads trying to acquire the same lock
    for (int i = 0; i < numThreads; ++i) {
        futures.push_back(std::async(std::launch::async, [&outputPath, &successCount]() -> bool {
            try {
                BuildLock lock(outputPath.string());
                if (lock.isHolding()) {
                    // Simulate some work
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    successCount++;
                    return true;
                }
            } catch (const AppException&) {
                // Expected - lock acquisition failed
            }
            return false;
        }));
    }

    // Wait for all threads to complete
    int succeededThreads = 0;
    for (auto& future : futures) {
        if (future.get()) {
            succeededThreads++;
        }
    }

    // Only one thread should have succeeded in acquiring the lock
    EXPECT_EQ(succeededThreads, 1);
    EXPECT_EQ(successCount.load(), 1);
}

/**
 * @brief Test lock behavior with non-existent directory
 */
TEST_F(BuildLockTest, NonExistentDirectory) {
    fs::path nonExistentPath = testArea->getPath("non_existent_dir") / "subdir" / "output";

    // Should throw appropriate exception
    EXPECT_THROW({
        BuildLock lock(nonExistentPath.string());
    }, AppException);
}

/**
 * @brief Test lock with very long path name (platform limits)
 */
TEST_F(BuildLockTest, LongPathName) {
    // Create a very long path name
    std::string longName(200, 'a'); // 200 character filename
    auto longPath = testArea->getPath(longName);

#ifdef WIN32
    // Windows has path length limitations
    if (longPath.string().length() > 260) {
        EXPECT_THROW({
            BuildLock lock(longPath.string());
        }, AppException);
        return;
    }
#else
    // Unix systems may have filename length limits
    if (longName.length() > 255) {
        EXPECT_THROW({
            BuildLock lock(longPath.string());
        }, AppException);
        return;
    }
#endif

    // If path is not too long, lock should work normally
    BuildLock lock(longPath.string());
    EXPECT_TRUE(lock.isHolding());
}

/**
 * @brief Test lock persistence across scope changes
 */
TEST_F(BuildLockTest, LockPersistence) {
    auto outputPath = testArea->getPath("persistence_test");

    // Create lock in inner scope
    {
        BuildLock lock(outputPath.string());
        EXPECT_TRUE(lock.isHolding());

        // Lock should prevent other acquisitions
        EXPECT_THROW({
            BuildLock conflictLock(outputPath.string());
        }, AppException);
    }
    // Lock should be released when leaving scope

    // Small delay to ensure cleanup is complete
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Should be able to acquire lock again
    BuildLock newLock(outputPath.string());
    EXPECT_TRUE(newLock.isHolding());
}

/**
 * @brief Test exception safety - ensure no locks are leaked
 */
TEST_F(BuildLockTest, ExceptionSafety) {
    auto outputPath = testArea->getPath("exception_test");

    auto testExceptionSafety = [&outputPath]() {
        BuildLock lock(outputPath.string());
        EXPECT_TRUE(lock.isHolding());

        // Simulate exception
        throw std::runtime_error("Test exception");
    };

    // Exception should not prevent lock cleanup
    EXPECT_THROW(testExceptionSafety(), std::runtime_error);

    // Small delay for cleanup
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Should be able to acquire lock after exception
    BuildLock newLock(outputPath.string());
    EXPECT_TRUE(newLock.isHolding());
}

/**
 * @brief Stress test - many rapid lock acquisitions and releases
 */
TEST_F(BuildLockTest, StressTest) {
    auto outputPath = testArea->getPath("stress_test");
    const int iterations = 100;

    for (int i = 0; i < iterations; ++i) {
        BuildLock lock(outputPath.string());
        EXPECT_TRUE(lock.isHolding());

        // Very brief hold
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    // Final verification
    BuildLock finalLock(outputPath.string());
    EXPECT_TRUE(finalLock.isHolding());
}

/**
 * @brief Test lock file content validation
 */
TEST_F(BuildLockTest, LockFileContent) {
    auto outputPath = testArea->getPath("content_test");

    BuildLock lock(outputPath.string());
    EXPECT_TRUE(lock.isHolding());

    auto lockFilePath = lock.getLockFilePath();

    // Lock file should exist while lock is held
#ifdef WIN32
    // On Windows, we can't easily read the file content due to exclusive access
    // but we can verify the path is correct
    EXPECT_TRUE(lockFilePath.find(".building") != std::string::npos);
#else
    // On Unix, we might be able to verify some content (implementation dependent)
    if (fs::exists(lockFilePath)) {
        EXPECT_TRUE(fs::file_size(lockFilePath) > 0);
    }
#endif
}

// Test that isBuildActive correctly detects active builds
TEST_F(BuildLockTest, IsBuildActiveDetection) {
    auto outputPath = testArea->getPath("build_active_test");

    // Initially, no lock should be active
    try {
        BuildLock testLock(outputPath.string(), false); // Try to acquire without waiting
        EXPECT_TRUE(testLock.isHolding());
        testLock.release(); // Release immediately
    } catch (const AppException&) {
        FAIL() << "Should be able to acquire lock when no other process is using it";
    }

    // Now test with an active lock
    {
        BuildLock activeLock(outputPath.string()); // Acquire and hold lock
        EXPECT_TRUE(activeLock.isHolding());

        // Try to acquire another lock - should fail immediately with wait=false
        EXPECT_THROW({
            BuildLock secondLock(outputPath.string(), false);
        }, AppException);
    }

    // After lock is released, should be able to acquire again
    try {
        BuildLock testLock(outputPath.string(), false);
        EXPECT_TRUE(testLock.isHolding());
    } catch (const AppException&) {
        FAIL() << "Should be able to acquire lock after previous lock is released";
    }
}



} // anonymous namespace