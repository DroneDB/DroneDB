/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "buildlock.h"
#include "exceptions.h"
#include "gtest/gtest.h"
#include "mio.h"
#include "test.h"
#include "testarea.h"
#include "helpers/process_test_utils.h"

#include <thread>
#include <iterator>
#include <chrono>
#include <fstream>
#include <future>
#include <latch>
#include <memory>
#include <mutex>
#include <vector>
#include <string>

#ifdef WIN32
#include <windows.h>
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <spawn.h>
#include <sstream>
extern char **environ;
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

    // Both platforms remove the lock when its last owner releases it.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(fs::exists(outputPath.string() + ".building"));
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
    // The holder keeps the lock until every thread has tried once: otherwise a thread that
    // starts late (a loaded machine) tries after the release and acquires it too. BuildLock
    // never blocks, so every thread reaches the latch.
    std::latch attempted(numThreads);

    // Launch multiple threads trying to acquire the same lock
    for (int i = 0; i < numThreads; ++i) {
        futures.push_back(std::async(std::launch::async, [&outputPath, &successCount, &attempted]() -> bool {
            std::unique_ptr<BuildLock> lock;
            try {
                lock = std::make_unique<BuildLock>(outputPath.string());
            } catch (const AppException&) {
                // Expected - lock acquisition failed
            }
            attempted.count_down();
            if (lock && lock->isHolding()) {
                attempted.wait();
                successCount++;
                return true;
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
 * @brief Rapid acquire/release contention must never yield two simultaneous holders
 *
 * Exercises the window where a contender opens the lock file just before the
 * holder unlinks it and would otherwise lock the orphaned inode.
 */
TEST_F(BuildLockTest, NoConcurrentHoldersUnderContention) {
    auto outputPath = testArea->getPath("contention_test");
    const int numThreads = 8;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::atomic<int> holders{0};
    std::atomic<int> maxHolders{0};
    std::atomic<int> acquisitions{0};
    std::atomic<int> unexpectedErrors{0};
    // Keep the first few swallowed exception messages to make failures self-diagnostic
    std::mutex errorsMx;
    std::vector<std::string> errorSamples;

    std::vector<std::thread> threads;
    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([&]() {
            while (std::chrono::steady_clock::now() < deadline) {
                try {
                    BuildLock lock(outputPath.string(), false);
                    const int now = ++holders;
                    int prev = maxHolders.load();
                    while (now > prev && !maxHolders.compare_exchange_weak(prev, now)) {}
                    acquisitions++;
                    std::this_thread::yield();
                    --holders;
                } catch (const BuildInProgressException&) {
                    // Expected under contention
                } catch (const std::exception& e) {
                    // Never let an exception escape the thread (std::terminate would
                    // kill the whole test binary)
                    unexpectedErrors++;
                    std::lock_guard<std::mutex> lk(errorsMx);
                    if (errorSamples.size() < 5) errorSamples.emplace_back(e.what());
                }
            }
        });
    }
    for (auto& t : threads) t.join();

    for (const auto& msg : errorSamples) {
        ADD_FAILURE() << "Unexpected exception: " << msg;
    }
    EXPECT_EQ(unexpectedErrors.load(), 0);
    EXPECT_GT(acquisitions.load(), 0);
    EXPECT_EQ(maxHolders.load(), 1);
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



/**
 * @brief Test that BuildLock correctly handles a pre-existing lock file on disk.
 *
 * Both platforms acquire exclusive ownership before replacing diagnostics.
 * File presence and PID contents alone do not imply a live holder.
 */
TEST_F(BuildLockTest, WaitMode_OrphanedLockFileOnDisk) {
    auto outputPath = testArea->getPath("stale_lock_test");

    // Simulate an orphaned lock: create .building file with a dead PID (no process holding it)
    std::string lockFile = outputPath.string() + ".building";
    {
        std::ofstream f(lockFile);
        f << "PID: 99\nProcess: Simulated stale\n";
    }
    ASSERT_TRUE(fs::exists(lockFile));

    // Both platforms now reclaim the orphan transparently.
    {
        BuildLock lock(outputPath.string());  // wait=true
        EXPECT_TRUE(lock.isHolding());
    }

    // Lock file should have been cleaned up after the BuildLock went out of scope.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(fs::exists(lockFile));
}

/**
 * @brief Test that wait=false reclaims an orphaned lock file with no live holder.
 *
 * An orphaned file is not considered "active build in progress" on either
 * platform; a live kernel lock or exclusive handle supplies exclusion.
 */
TEST_F(BuildLockTest, NoWaitMode_ReclaimsOrphanedLockFile) {
    auto outputPath = testArea->getPath("orphan_lock_test");

    // Create an orphaned lock file (no process holds the kernel lock)
    std::string lockFile = outputPath.string() + ".building";
    {
        std::ofstream f(lockFile);
        f << "PID: 99\n";
    }
    ASSERT_TRUE(fs::exists(lockFile));

    // The orphan has no live holder, so wait=false reclaims it on both platforms.
    {
        BuildLock lock(outputPath.string(), false);
        EXPECT_TRUE(lock.isHolding());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(fs::exists(lockFile));
}

/**
 * @brief End-to-end recovery scenario: a "crashed" build leaves an orphan
 * lock file behind; a subsequent process must be able to build without
 * any special force flag or manual cleanup on either platform.
 */
TEST_F(BuildLockTest, ForceBuildScenario_StaleLockRecovery) {
    auto outputPath = testArea->getPath("force_build_scenario");

    // Step 1: Simulate crashed build leaving a lock file with dead PID
    std::string lockFile = outputPath.string() + ".building";
    {
        std::ofstream f(lockFile);
        f << "PID: 99\nProcess: Crashed build\n";
    }
    ASSERT_TRUE(fs::exists(lockFile));

    // Both modes recover transparently because the previous owner is gone.
    {
        BuildLock recoveryLock(outputPath.string(), false);
        EXPECT_TRUE(recoveryLock.isHolding());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(fs::exists(lockFile));
}

#ifdef WIN32
// Test helper handles are always closed, including after fatal assertions.
class WindowsChild {
public:
    PROCESS_INFORMATION info{};
    ~WindowsChild() {
        if (info.hProcess) {
            if (WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT) {
                TerminateProcess(info.hProcess, 1);
                WaitForSingleObject(info.hProcess, 5000);
            }
            CloseHandle(info.hProcess);
        }
        if (info.hThread) CloseHandle(info.hThread);
    }
};

TEST_F(BuildLockTest, MultiProcess_KernelReleaseOnKill) {
    const auto outputPath = testArea->getPath("multi_proc_kill");
    const auto ready = fs::path(outputPath.string() + ".child-ready");
    io::assureIsRemoved(ready);
    const auto helper = testChildPath();
    std::wstring command = L"\"" + helper.wstring() + L"\" hold-lock \"" +
                           outputPath.wstring() + L"\" \"" + ready.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    WindowsChild child;
    ASSERT_TRUE(CreateProcessW(helper.wstring().c_str(), command.data(), nullptr, nullptr,
                              FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child.info));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!fs::exists(ready) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ASSERT_TRUE(fs::exists(ready)) << "Child did not acquire lock in time";
    EXPECT_THROW({ BuildLock concurrent(outputPath.string(), false); }, BuildInProgressException);

    // A forced exit bypasses the BuildLock destructor. The kernel must close
    // the exclusive handle and apply FILE_FLAG_DELETE_ON_CLOSE.
    ASSERT_TRUE(TerminateProcess(child.info.hProcess, 1));
    ASSERT_EQ(WaitForSingleObject(child.info.hProcess, 5000), WAIT_OBJECT_0);
    EXPECT_FALSE(fs::exists(outputPath.string() + ".building"));
    BuildLock recovered(outputPath.string(), false);
    EXPECT_TRUE(recovered.isHolding());
}

TEST_F(BuildLockTest, LiveExclusiveHandleDoesNotTruncateDiagnostics) {
    const auto outputPath = testArea->getPath("exclusive_diagnostics");
    const auto lockFile = fs::path(outputPath.string() + ".building");
    const std::string diagnostic = "PID: 99\nLive external holder\n";
    { std::ofstream out(lockFile.string()); out << diagnostic; }
    HANDLE holder = CreateFileW(lockFile.wstring().c_str(), GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(holder, INVALID_HANDLE_VALUE);
    EXPECT_THROW({ BuildLock rejected(outputPath.string(), false); }, BuildInProgressException);
    EXPECT_THROW({ BuildLock rejected(outputPath.string(), true); }, BuildInProgressException);
    CloseHandle(holder);
    std::ifstream in(lockFile.string());
    const std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(contents, diagnostic);
    in.close();
    // After the external handle closes, its file is an acquirable orphan.
    BuildLock recovered(outputPath.string(), false);
    EXPECT_TRUE(recovered.isHolding());
}
#endif

/**
 * @brief Multi-process recovery: after a child process holding the lock is
 * forcefully killed (SIGKILL), the parent must be able to reacquire the lock
 * immediately without intervention. This validates the kernel-managed
 * release-on-death property that was the whole point of the refactor.
 */
#ifndef WIN32
TEST_F(BuildLockTest, MultiProcess_KernelReleaseOnKill) {
    auto outputPath = testArea->getPath("multi_proc_kill");
    // TestArea is reused across runs; a leftover sentinel would skip the wait below
    io::assureIsRemoved(outputPath.string() + ".child-ready");

    pid_t child = fork();
    ASSERT_NE(child, -1);

    if (child == 0) {
        // Child: acquire lock and sleep forever until killed
        try {
            BuildLock lock(outputPath.string());
            // Signal parent that the lock is held by creating a sentinel file
            std::ofstream(outputPath.string() + ".child-ready").close();
            // Hold the lock until SIGKILL
            for (;;) ::pause();
        } catch (...) {
            ::_exit(1);
        }
        ::_exit(0);
    }

    // Parent: wait for child to acquire the lock
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!fs::exists(outputPath.string() + ".child-ready") &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_TRUE(fs::exists(outputPath.string() + ".child-ready"))
        << "Child did not acquire lock in time";

    // Parent attempt to lock must fail while child is alive
    EXPECT_THROW({
        BuildLock concurrent(outputPath.string(), false);
    }, BuildInProgressException);

    // Kill the child with SIGKILL (no graceful cleanup, no destructors)
    ASSERT_EQ(::kill(child, SIGKILL), 0);
    int status = 0;
    ::waitpid(child, &status, 0);

    // Parent must be able to immediately reacquire the lock, even though the
    // .building file may still be on disk (the kernel released the lock).
    {
        BuildLock recovered(outputPath.string(), false);
        EXPECT_TRUE(recovered.isHolding());
    }
}
#endif

/**
 * @brief The lock descriptor is close-on-exec: a child process spawned while a
 * BuildLock is held (with a plain posix_spawn that inherits every non-CLOEXEC
 * descriptor) must not receive it, so it can never keep the lock alive.
 */
#ifdef __linux__
TEST_F(BuildLockTest, LockFdNotInheritedByChildren) {
    auto outputPath = testArea->getPath("cloexec_lock");
    BuildLock lock(outputPath.string());
    ASSERT_TRUE(lock.isHolding());
    const std::string lockFile = fs::canonical(lock.getLockFilePath()).string();

    // Parent side: the descriptor pointing at the lock file has FD_CLOEXEC.
    int lockFd = -1;
    for (const auto& e : fs::directory_iterator("/proc/self/fd")) {
        std::error_code ec;
        const auto target = fs::read_symlink(e.path(), ec);
        if (!ec && target.string() == lockFile) lockFd = std::stoi(e.path().filename().string());
    }
    ASSERT_GE(lockFd, 0) << "lock descriptor not found in /proc/self/fd";
    const int flags = ::fcntl(lockFd, F_GETFD);
    ASSERT_NE(flags, -1);
    EXPECT_TRUE(flags & FD_CLOEXEC);

    // Child side: list the descriptor targets of a spawned shell.
    const auto listing = testArea->getPath("child_fds.txt");
    // (`|| true`: the descriptor the shell used to expand the glob is gone by now)
    const std::string script = "for f in /proc/$$/fd/*; do readlink \"$f\" || true; done > \"$1\"";
    std::vector<std::string> args = {"sh", "-c", script, "sh", listing.string()};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    ASSERT_EQ(::posix_spawn(&pid, "/bin/sh", nullptr, nullptr, argv.data(), environ), 0);
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    std::ifstream in(listing.string());
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string targets = ss.str();
    EXPECT_FALSE(targets.empty());
    EXPECT_EQ(targets.find(lockFile), std::string::npos) << "child inherited the lock fd:\n" << targets;
}
#endif

// ---- BuildLock::isHeld / removeIfStale (DQ-08) -----------------------------------------

std::string readWholeFile(const fs::path& p) {
    std::ifstream in(p.string(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Lock file of `outputPath` with known diagnostic contents and no live holder: what a holder
// killed with SIGKILL (or a copied dataset) leaves behind.
fs::path writeOrphanLockFile(const fs::path& outputPath, const std::string& contents) {
    const fs::path lockFile = outputPath.string() + ".building";
    std::ofstream(lockFile.string(), std::ios::binary) << contents;
    return lockFile;
}

// The test child in "hold-lock" mode: another process holding the BuildLock of an output path
// until it is killed (the kernel then releases the lock, as for a crashed build).
class LockHolderChild {
public:
    explicit LockHolderChild(const fs::path& outputPath)
        : ready_(outputPath.string() + ".child-ready") {
        io::assureIsRemoved(ready_);
        const auto helper = testChildPath();
#ifdef WIN32
        std::wstring command = L"\"" + helper.wstring() + L"\" hold-lock \"" +
                               outputPath.wstring() + L"\" \"" + ready_.wstring() + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        started_ = CreateProcessW(helper.wstring().c_str(), command.data(), nullptr, nullptr,
                                  FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info_) != 0;
#else
        std::vector<std::string> args = {helper.string(), "hold-lock", outputPath.string(),
                                         ready_.string()};
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        started_ = ::posix_spawn(&pid_, helper.c_str(), nullptr, nullptr, argv.data(), environ) == 0;
#endif
    }

    ~LockHolderChild() { kill(); }

    LockHolderChild(const LockHolderChild&) = delete;
    LockHolderChild& operator=(const LockHolderChild&) = delete;

    // The child writes the ready file only after it acquired the lock.
    bool waitUntilHolding() const {
        if (!started_) return false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!fs::exists(ready_) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return fs::exists(ready_);
    }

    // Forced exit (no destructors run) and reap: the lock is released by the kernel.
    void kill() {
#ifdef WIN32
        if (info_.hProcess) {
            TerminateProcess(info_.hProcess, 1);
            WaitForSingleObject(info_.hProcess, 5000);
            CloseHandle(info_.hProcess);
            info_.hProcess = nullptr;
        }
        if (info_.hThread) {
            CloseHandle(info_.hThread);
            info_.hThread = nullptr;
        }
#else
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            int status = 0;
            ::waitpid(pid_, &status, 0);
            pid_ = 0;
        }
#endif
    }

private:
    fs::path ready_;
    bool started_ = false;
#ifdef WIN32
    PROCESS_INFORMATION info_{};
#else
    pid_t pid_ = 0;
#endif
};

TEST_F(BuildLockTest, IsHeld_NoLockFile_NotHeldAndNothingCreated) {
    const auto outputPath = testArea->getPath("is_held_absent");
    const fs::path lockFile = outputPath.string() + ".building";
    io::assureIsRemoved(lockFile);

    EXPECT_FALSE(BuildLock::isHeld(outputPath.string()));
    EXPECT_FALSE(fs::exists(lockFile)) << "probing must not create the lock file";

    // A missing parent folder is "not held" too, not an error
    EXPECT_FALSE(BuildLock::isHeld((outputPath / "missing" / "cog").string()));
    EXPECT_FALSE(fs::exists(outputPath / "missing"));
}

TEST_F(BuildLockTest, IsHeld_StaleLockFile_NotHeldAndLeftUntouched) {
    const auto outputPath = testArea->getPath("is_held_stale");
    const std::string diagnostic = "PID: 99\nProcess: crashed build\n";
    const fs::path lockFile = writeOrphanLockFile(outputPath, diagnostic);
    const auto writeTime = fs::last_write_time(lockFile);

    EXPECT_FALSE(BuildLock::isHeld(outputPath.string()));
    EXPECT_FALSE(BuildLock::isHeld(outputPath.string()));

    ASSERT_TRUE(fs::exists(lockFile)) << "probing must not remove a stale lock file";
    EXPECT_EQ(readWholeFile(lockFile), diagnostic) << "probing must not truncate or rewrite it";
    EXPECT_EQ(fs::last_write_time(lockFile), writeTime);

    // The stale file stays reclaimable by a real acquisition
    BuildLock lock(outputPath.string(), false);
    EXPECT_TRUE(lock.isHolding());
}

TEST_F(BuildLockTest, IsHeld_HolderInThisProcess) {
    const auto outputPath = testArea->getPath("is_held_in_process");
    io::assureIsRemoved(outputPath.string() + ".building");
    {
        BuildLock lock(outputPath.string(), false);
#ifndef WIN32
        // (On Windows the holder's no-share handle keeps everyone else from reading it)
        const std::string diagnostic = readWholeFile(lock.getLockFilePath());
        EXPECT_FALSE(diagnostic.empty());
#endif

        EXPECT_TRUE(BuildLock::isHeld(outputPath.string()));
        EXPECT_TRUE(lock.isHolding());
#ifndef WIN32
        EXPECT_EQ(readWholeFile(lock.getLockFilePath()), diagnostic);
#endif
        // Probing never steals or weakens the lock: a second acquisition still fails
        EXPECT_THROW({ BuildLock second(outputPath.string(), false); }, BuildInProgressException);
    }
    EXPECT_FALSE(BuildLock::isHeld(outputPath.string()));
}

TEST_F(BuildLockTest, IsHeld_HolderInAnotherProcessThenReleased) {
    const auto outputPath = testArea->getPath("is_held_other_process");
    const fs::path lockFile = outputPath.string() + ".building";
    io::assureIsRemoved(lockFile);

    LockHolderChild child(outputPath);
    ASSERT_TRUE(child.waitUntilHolding()) << "child did not acquire the lock in time";
#ifndef WIN32
    // (On Windows the holder's no-share handle keeps everyone else from reading it)
    const std::string diagnostic = readWholeFile(lockFile);
    EXPECT_FALSE(diagnostic.empty());
#endif

    EXPECT_TRUE(BuildLock::isHeld(outputPath.string()));
    EXPECT_TRUE(BuildLock::isHeld(outputPath.string()));
#ifndef WIN32
    EXPECT_EQ(readWholeFile(lockFile), diagnostic) << "probing must not touch the holder's file";
#endif
    EXPECT_THROW({ BuildLock concurrent(outputPath.string(), false); }, BuildInProgressException);

    // Kill the holder: the kernel releases the lock (and on Windows deletes the file).
    child.kill();
    EXPECT_FALSE(BuildLock::isHeld(outputPath.string()));
#ifndef WIN32
    // On Unix the killed holder leaves its file behind; the probe keeps it
    ASSERT_TRUE(fs::exists(lockFile));
    EXPECT_EQ(readWholeFile(lockFile), diagnostic);
#endif
    BuildLock recovered(outputPath.string(), false);
    EXPECT_TRUE(recovered.isHolding());
}

TEST_F(BuildLockTest, RemoveIfStale_RemovesOnlyAStaleLockFile) {
    const auto outputPath = testArea->getPath("remove_if_stale");
    const fs::path lockFile = outputPath.string() + ".building";
    io::assureIsRemoved(lockFile);

    // Nothing to remove, nothing created
    EXPECT_TRUE(BuildLock::removeIfStale(outputPath.string()));
    EXPECT_FALSE(fs::exists(lockFile));

    // Stale: removed
    writeOrphanLockFile(outputPath, "PID: 99\n");
    EXPECT_TRUE(BuildLock::removeIfStale(outputPath.string()));
    EXPECT_FALSE(fs::exists(lockFile));

    // Live: kept, and the holder keeps the lock
    BuildLock lock(outputPath.string(), false);
    EXPECT_FALSE(BuildLock::removeIfStale(outputPath.string()));
    EXPECT_TRUE(fs::exists(lockFile));
    EXPECT_TRUE(BuildLock::isHeld(outputPath.string()));
}

} // anonymous namespace
