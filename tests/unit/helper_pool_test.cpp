/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The generic pool of helper processes (helper_pool.h), on a stand-in that speaks in lines and
// breaks the way each request asks (tests/helpers/fake_line_helper.cpp).
#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "cancellation.h"
#include "dbops.h"
#include "exceptions.h"
#include "fs.h"
#include "gtest/gtest.h"
#include "helper_pool.h"
#include "mio.h"
#include "process_config.h"
#include "testarea.h"
#include "utils.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace
{
    using namespace ddb;
    using helper::Pool;
    using helper::PoolOptions;
    using std::chrono::milliseconds;
    using std::chrono::seconds;
    using Clock = std::chrono::steady_clock;

    // How long a step that must succeed may take before the test calls it hung
    constexpr seconds kPatience(30);

    // The address space limit cannot be tested under a sanitizer: the instrumented child reserves
    // terabytes of shadow memory at start, so it dies from any limit, whatever it allocates.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SKIP_UNDER_SANITIZER() GTEST_SKIP() << "an address space limit does not fit a sanitizer build"
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define SKIP_UNDER_SANITIZER() GTEST_SKIP() << "an address space limit does not fit a sanitizer build"
#endif
#endif
#ifndef SKIP_UNDER_SANITIZER
#define SKIP_UNDER_SANITIZER() \
    do                         \
    {                          \
    } while (0)
#endif

    fs::path fakeLinePath()
    {
        return io::getExeFolderPath() / DDB_FAKE_LINE_NAME;
    }

    // The accept rule of the fake: "fake-line 1.x"
    helper::VersionVerdict acceptFakeVersion(const std::string &output)
    {
        if (output.rfind("fake-line 1.", 0) != 0)
            return {false, "it speaks \"" + helper::lastBytes(output, 100) + "\", this test needs fake-line 1.x"};
        return {true, helper::lastBytes(output, 100)};
    }

    // A pool of the fake helper with room for `limit` helpers
    PoolOptions fakeOptions(size_t limit)
    {
        PoolOptions options;
        options.name = "fake-line";
        options.locate = [] { return fakeLinePath(); };
        options.validateVersion = acceptFakeVersion;
        options.limit = [limit] { return limit; };
        return options;
    }

    Clock::time_point soon()
    {
        return Clock::now() + kPatience;
    }

    // One request, one response, and the helper goes back to the pool
    std::string ask(Pool &pool, const std::string &request)
    {
        Pool::Lease lease = pool.acquire();
        std::string response = lease.exchange(request, soon());
        lease.keep();
        return response;
    }

    // Whether a process is running (not a zombie), by the id a helper had
    bool processAlive(unsigned long pid)
    {
#ifdef _WIN32
        HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
        if (handle == nullptr)
            return false;
        const bool running = WaitForSingleObject(handle, 0) == WAIT_TIMEOUT;
        CloseHandle(handle);
        return running;
#elif defined(__linux__)
        std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
        std::string line;
        if (!std::getline(stat, line))
            return false;
        // "<pid> (<name>) <state> ...": the name may hold spaces and parentheses
        const size_t close = line.rfind(')');
        return close != std::string::npos && close + 2 < line.size() && line[close + 2] != 'Z' &&
               line[close + 2] != 'X';
#else
        return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
    }

    // Kills a process by the id a helper had, behind the back of its pool (as the OOM killer or
    // an operator would)
    void killProcess(unsigned long pid)
    {
#ifdef _WIN32
        HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
        ASSERT_NE(handle, nullptr);
        TerminateProcess(handle, 9);
        CloseHandle(handle);
#else
        ASSERT_EQ(::kill(static_cast<pid_t>(pid), SIGKILL), 0);
#endif
    }

    // The lines a file holds (the version log of the fake)
    size_t linesOf(const fs::path &file)
    {
        std::ifstream in(file);
        size_t count = 0;
        for (std::string line; std::getline(in, line);)
            count++;
        return count;
    }

    // Joins its threads when it goes out of scope, so a failed assertion cannot leave one running
    class Threads
    {
      public:
        template <typename F>
        void start(F &&body)
        {
            threads_.emplace_back(std::forward<F>(body));
        }
        void join()
        {
            for (auto &thread : threads_)
                if (thread.joinable())
                    thread.join();
        }
        ~Threads() { join(); }

      private:
        std::vector<std::thread> threads_;
    };

    // ------------------------------------------------------------------ requests

    TEST(HelperPool, startsNoHelperBeforeTheFirstLease)
    {
        Pool pool(fakeOptions(4));
        EXPECT_EQ(pool.startedHelpers(), 0u);
        EXPECT_EQ(pool.idleHelpers(), 0u);
        EXPECT_EQ(ask(pool, "echo hello"), "hello");
        EXPECT_EQ(pool.startedHelpers(), 1u);
        EXPECT_EQ(pool.idleHelpers(), 1u);
        EXPECT_EQ(pool.busyHelpers(), 0u);
        EXPECT_EQ(pool.requestsSent(), 1u);
    }

    TEST(HelperPool, aKeptHelperServesTheNextRequest)
    {
        Pool pool(fakeOptions(4));
        const std::string first = ask(pool, "pid");
        for (int i = 0; i < 10; i++)
            ASSERT_EQ(ask(pool, "pid"), first);
        EXPECT_EQ(pool.startedHelpers(), 1u);
    }

    TEST(HelperPool, aLeaseThatIsNotKeptClosesItsHelper)
    {
        Pool pool(fakeOptions(4));
        unsigned long pid = 0;
        {
            Pool::Lease lease = pool.acquire();
            pid = lease.pid();
            EXPECT_EQ(lease.exchange("echo ignored", soon()), "ignored");
        }
        EXPECT_EQ(pool.idleHelpers(), 0u) << "the caller never said the response was good";
        EXPECT_EQ(pool.busyHelpers(), 0u);
        EXPECT_FALSE(processAlive(pid));
        EXPECT_EQ(ask(pool, "echo again"), "again");
        EXPECT_EQ(pool.startedHelpers(), 2u);
    }

    TEST(HelperPool, theLimitIsReadWhenACallerTakesAHelper)
    {
        std::atomic<size_t> limit{1};
        PoolOptions options = fakeOptions(1);
        options.limit = [&] { return limit.load(); };
        Pool pool(options);

        std::optional<Pool::Lease> first = pool.tryAcquire();
        ASSERT_TRUE(first.has_value());
        EXPECT_FALSE(pool.tryAcquire().has_value()) << "limit 1: the pool is full";
        EXPECT_EQ(pool.startedHelpers(), 1u);

        limit = 2;
        std::optional<Pool::Lease> second = pool.tryAcquire();
        ASSERT_TRUE(second.has_value()) << "the raised limit counts at once";
        EXPECT_FALSE(pool.tryAcquire().has_value());
        EXPECT_EQ(pool.busyHelpers(), 2u);
        EXPECT_EQ(pool.startedHelpers(), 2u);

        first.reset();
        second.reset();
        EXPECT_EQ(pool.busyHelpers(), 0u);
    }

    TEST(HelperPool, callersAreServedInTheOrderTheyCame)
    {
        Pool pool(fakeOptions(1));
        std::optional<Pool::Lease> holder = pool.acquire();
        holder->exchange("echo held", soon());
        holder->keep();

        constexpr size_t kCallers = 5;
        std::mutex mutex;
        std::vector<size_t> served;
        Threads threads;
        for (size_t i = 0; i < kCallers; i++)
        {
            threads.start([&, i] {
                Pool::Lease lease = pool.acquire();
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    served.push_back(i);
                }
                lease.exchange("echo " + std::to_string(i), soon());
                lease.keep();
            });
            // The next caller starts once this one is queued, so the order of arrival is known
            ASSERT_TRUE(pool.awaitWaiters(i + 1, kPatience));
        }
        EXPECT_EQ(pool.queuedCallers(), kCallers);

        holder.reset();
        threads.join();
        const std::vector<size_t> expected = {0, 1, 2, 3, 4};
        EXPECT_EQ(served, expected);
        EXPECT_EQ(pool.startedHelpers(), 1u) << "one helper served them all, one after the other";
    }

    TEST(HelperPool, theDefaultLimitIsTheCpuPoolSizePlusOne)
    {
        PoolOptions options = fakeOptions(1);
        options.limit = nullptr;
        Pool pool(options);
        EXPECT_EQ(pool.currentLimit(), ProcessConfig::get().cpuPoolSize() + 1);

        ScopedProcessConfig configured(R"({"cpuPoolSize": 3})");
        EXPECT_EQ(pool.currentLimit(), 4u) << "read when a caller takes a helper, so DDBConfigure applies at once";

        // The pool really holds that many: the fourth helper is started, the fifth is not
        std::vector<Pool::Lease> leases;
        for (size_t i = 0; i < 4; i++)
        {
            std::optional<Pool::Lease> lease = pool.tryAcquire();
            ASSERT_TRUE(lease.has_value()) << "helper " << i;
            leases.push_back(std::move(*lease));
        }
        EXPECT_FALSE(pool.tryAcquire().has_value());
    }

    // ------------------------------------------------------------------ idle helpers

    TEST(HelperPool, aHelperIdleForMoreThanAMinuteIsClosedWhenTheNextOneIsTaken)
    {
        std::atomic<int64_t> offsetSeconds{0};
        PoolOptions options = fakeOptions(1);
        options.now = [&] { return Clock::now() + seconds(offsetSeconds.load()); };
        Pool pool(options);

        ask(pool, "echo a");
        const auto first = pool.idleHelperPids();
        ASSERT_EQ(first.size(), 1u);

        offsetSeconds = 59;
        ask(pool, "echo b");
        EXPECT_EQ(pool.startedHelpers(), 1u) << "59 s idle: the helper is reused";
        EXPECT_EQ(pool.idleHelperPids(), first);

        offsetSeconds = 59 + 61;
        ask(pool, "echo c");
        EXPECT_EQ(pool.startedHelpers(), 2u) << "61 s idle: the helper was closed and a new one started";
        EXPECT_FALSE(processAlive(first[0]));
        EXPECT_NE(pool.idleHelperPids(), first);
    }

    TEST(HelperPool, aHelperThatDiedWhileIdleIsReplacedWithoutAFailure)
    {
        Pool pool(fakeOptions(1));
        ask(pool, "echo a");
        const auto pids = pool.idleHelperPids();
        ASSERT_EQ(pids.size(), 1u);
        killProcess(pids[0]);
        // The pool learns of the death from the process: a wait for a condition of another process
        const auto deadline = Clock::now() + kPatience;
        while (processAlive(pids[0]) && Clock::now() < deadline)
            std::this_thread::sleep_for(milliseconds(2));
        ASSERT_FALSE(processAlive(pids[0]));

        std::string response;
        ASSERT_NO_THROW(response = ask(pool, "echo b")) << "a helper that died while idle is not the fault of the request";
        EXPECT_EQ(response, "b");
        EXPECT_EQ(pool.startedHelpers(), 2u);
        EXPECT_EQ(pool.busyHelpers(), 0u);
        EXPECT_EQ(pool.idleHelpers(), 1u);
    }

    // ------------------------------------------------------------------ the version

    // A copy of the fake in the test area: its size and time can change without touching the
    // build output
    fs::path copyOfFake(TestArea &area, const std::string &name)
    {
        const fs::path copy = area.getFolder() / name;
        fs::copy_file(fakeLinePath(), copy, fs::copy_options::overwrite_existing);
        return copy;
    }

    TEST(HelperPool, theVersionIsAskedOncePerProgramFile)
    {
        TestArea area("HelperPool.versionCache", true);
        const fs::path log = area.getFolder() / "version.log";
        EnvVarGuard logEnv("DDB_FAKE_LINE_VERSION_LOG", io::toUtf8(log));
        const fs::path program = copyOfFake(area, "fake-line-copy" + fakeLinePath().extension().string());

        PoolOptions options = fakeOptions(3);
        options.locate = [&] { return program; };
        Pool pool(options);

        // Three helpers at once: the version is asked for the first and the others reuse the answer
        {
            std::vector<Pool::Lease> leases;
            for (int i = 0; i < 3; i++)
                leases.push_back(pool.acquire());
            EXPECT_EQ(pool.startedHelpers(), 3u);
        }
        EXPECT_EQ(linesOf(log), 1u);

        // The file changed (a newer time): the answer is not trusted any more
        fs::last_write_time(program, fs::last_write_time(program) + std::chrono::hours(1));
        pool.shutdown();
        ask(pool, "echo changed");
        EXPECT_EQ(linesOf(log), 2u);
        ask(pool, "echo unchanged");
        EXPECT_EQ(linesOf(log), 2u) << "asked again only for the file as it is now";
    }

    TEST(HelperPool, aVersionTheValidatorRefusesIsAHelperUnavailableAndIsAskedAgain)
    {
        TestArea area("HelperPool.versionRefused", true);
        const fs::path log = area.getFolder() / "version.log";
        EnvVarGuard logEnv("DDB_FAKE_LINE_VERSION_LOG", io::toUtf8(log));
        EnvVarGuard version("DDB_FAKE_LINE_VERSION", "fake-line 2.0");

        Pool pool(fakeOptions(1));
        try
        {
            pool.acquire();
            FAIL() << "a version the validator refuses must not give a helper";
        }
        catch (const HelperUnavailableException &e)
        {
            const std::string message = e.what();
            EXPECT_NE(message.find("fake-line"), std::string::npos) << message;
            EXPECT_NE(message.find("fake-line 2.0"), std::string::npos) << message;
            EXPECT_NE(message.find(io::toUtf8(fakeLinePath())), std::string::npos) << message;
        }
        EXPECT_EQ(pool.busyHelpers(), 0u) << "the place went back";
        EXPECT_EQ(pool.startedHelpers(), 0u);

        EXPECT_THROW(pool.acquire(), HelperUnavailableException);
        EXPECT_EQ(linesOf(log), 2u) << "a refused program may have been fixed meanwhile";

        version.restore();
        EXPECT_EQ(ask(pool, "echo fixed"), "fixed");
    }

    TEST(HelperPool, theAdapterChoosesTheExceptionOfAProgramThatCannotBeUsed)
    {
        PoolOptions options = fakeOptions(1);
        options.locate = [] { return fs::path(); };
        options.raiseUnavailable = [](const std::string &message) { throw MetadataHelperException(message); };
        Pool pool(options);
        EXPECT_THROW(pool.acquire(), MetadataHelperException);
        EXPECT_THROW(pool.acquire(), HelperUnavailableException) << "which is one";
    }

    TEST(HelperPool, aMissingProgramIsAHelperUnavailable)
    {
        PoolOptions options = fakeOptions(1);
        options.locate = [] { return fs::path(); };
        {
            Pool pool(options);
            try
            {
                pool.acquire();
                FAIL() << "there is no program";
            }
            catch (const HelperUnavailableException &e)
            {
                EXPECT_NE(std::string(e.what()).find("fake-line was not found"), std::string::npos) << e.what();
            }
        }

        options.missingMessage = [] { return std::string("set FAKE_LINE_PATH to the program"); };
        Pool described(options);
        try
        {
            described.acquire();
            FAIL() << "there is no program";
        }
        catch (const HelperUnavailableException &e)
        {
            EXPECT_NE(std::string(e.what()).find("FAKE_LINE_PATH"), std::string::npos) << e.what();
        }
    }

    TEST(HelperPool, aProgramFileThatDoesNotExistIsAHelperUnavailableAndKeepsThePlace)
    {
        TestArea area("HelperPool.noFile", true);
        PoolOptions options = fakeOptions(1);
        options.locate = [&] { return area.getFolder() / "no-such-program"; };
        Pool pool(options);
        EXPECT_THROW(pool.acquire(), HelperUnavailableException);
        EXPECT_THROW(pool.acquire(), HelperUnavailableException) << "limit 1: a leaked place would hang here";
        EXPECT_EQ(pool.busyHelpers(), 0u);
    }

    // ------------------------------------------------------------------ failures

    // A helper that breaks is killed, dropped, and never reused
    void expectKilledAndReplaced(Pool &pool, unsigned long pid)
    {
        EXPECT_FALSE(processAlive(pid)) << "the broken helper is gone";
        EXPECT_EQ(pool.busyHelpers(), 0u) << "its place went back";
        EXPECT_EQ(pool.idleHelpers(), 0u) << "and it did not go back to the pool";
        const size_t started = pool.startedHelpers();
        EXPECT_EQ(ask(pool, "echo next"), "next");
        EXPECT_EQ(pool.startedHelpers(), started + 1) << "the next request starts a new helper";
    }

    TEST(HelperPool, aHelperThatEndsWithoutAnsweringIsKilledAndTheRequestFails)
    {
        Pool pool(fakeOptions(1));
        std::optional<Pool::Lease> lease = pool.acquire();
        const unsigned long pid = lease->pid();
        lease->setContext("serving a test");
        EXPECT_THROW(lease->exchange("crash", soon()), helper::Failure);
        lease.reset();
        expectKilledAndReplaced(pool, pid);
    }

    TEST(HelperPool, aHelperThatDoesNotAnswerInTimeIsKilledAndTheRequestFails)
    {
        Pool pool(fakeOptions(1));
        std::optional<Pool::Lease> lease = pool.acquire();
        const unsigned long pid = lease->pid();
        const auto started = Clock::now();
        try
        {
            lease->exchange("hang", Clock::now() + milliseconds(300));
            FAIL() << "a helper that hangs does not answer";
        }
        catch (const helper::Failure &e)
        {
            EXPECT_NE(std::string(e.what()).find("no response"), std::string::npos) << e.what();
        }
        EXPECT_LT(Clock::now() - started, seconds(10));
        lease.reset();
        expectKilledAndReplaced(pool, pid);
    }

    TEST(HelperPool, aResponseLongerThanTheLimitKillsTheHelper)
    {
        PoolOptions options = fakeOptions(1);
        options.maxResponseBytes = 1024 * 1024;
        Pool pool(options);

        EXPECT_EQ(ask(pool, "huge 1000").size(), 1000u);
        EXPECT_EQ(pool.startedHelpers(), 1u);

        std::optional<Pool::Lease> lease = pool.acquire();
        const unsigned long pid = lease->pid();
        try
        {
            lease->exchange("huge 3000000", soon());
            FAIL() << "a line of 3 MB is over the limit";
        }
        catch (const helper::Failure &e)
        {
            EXPECT_NE(std::string(e.what()).find("longer than 1024 KiB"), std::string::npos) << e.what();
        }
        lease.reset();
        expectKilledAndReplaced(pool, pid);
    }

    TEST(HelperPool, aResponseTheCallerRefusesKillsTheHelper)
    {
        Pool pool(fakeOptions(1));
        std::optional<Pool::Lease> lease = pool.acquire();
        const unsigned long pid = lease->pid();
        EXPECT_EQ(lease->exchange("echo not what the protocol says", soon()), "not what the protocol says");
        try
        {
            lease->fail("fake-line sent an invalid response", false);
        }
        catch (const helper::Failure &e)
        {
            EXPECT_STREQ(e.what(), "fake-line sent an invalid response");
        }
        lease.reset();
        expectKilledAndReplaced(pool, pid);
    }

    TEST(HelperPool, anAbandonedRequestKillsTheHelper)
    {
        Pool pool(fakeOptions(1));
        std::optional<Pool::Lease> lease = pool.acquire();
        const unsigned long pid = lease->pid();
        lease->send("echo never read", soon());
        lease->abandon("the caller left");
        lease.reset();
        expectKilledAndReplaced(pool, pid);
    }

    // ------------------------------------------------------------------ cancellation

    TEST(HelperPool, aCanceledCallerStopsWaitingForTheResponseAndTheHelperIsKilled)
    {
        Pool pool(fakeOptions(1));
        std::optional<Pool::Lease> lease = pool.acquire();
        const unsigned long pid = lease->pid();
        lease->send("hang", soon());

        auto token = cancel::TokenRef::adopt(cancel::Token::create());
        token.get()->cancel("canceled by the test");
        const auto started = Clock::now();
        {
            cancel::ScopedThreadToken scope(token);
            // A deadline of a minute: the wait ends because of the cancellation, within a
            // slice of half a second and some
            EXPECT_THROW(lease->receive(Clock::now() + seconds(60)), CanceledException);
        }
        EXPECT_LT(Clock::now() - started, seconds(5));
        lease.reset();
        expectKilledAndReplaced(pool, pid);
    }

    TEST(HelperPool, aCancellationDuringAnExchangeFromAnotherThreadKillsTheHelper)
    {
        Pool pool(fakeOptions(1));
        auto token = cancel::TokenRef::adopt(cancel::Token::create());
        std::atomic<unsigned long> pid{0};
        std::atomic<bool> canceled{false};
        Clock::time_point finished;

        Threads threads;
        threads.start([&] {
            cancel::ScopedThreadToken scope(token);
            try
            {
                Pool::Lease lease = pool.acquire();
                pid = lease.pid();
                lease.exchange("hang", Clock::now() + seconds(60));
            }
            catch (const CanceledException &)
            {
                canceled = true;
            }
            finished = Clock::now();
        });
        while (pool.requestsSent() == 0)
            std::this_thread::sleep_for(milliseconds(2));
        const auto asked = Clock::now();
        token.get()->cancel("canceled by the test");
        threads.join();

        EXPECT_TRUE(canceled);
        EXPECT_LT(finished - asked, seconds(5)) << "the wait looks every half second";
        expectKilledAndReplaced(pool, pid);
    }

    // ------------------------------------------------------------------ shutdown

    TEST(HelperPool, shutdownClosesTheIdleHelpersAndThePoolWorksAgain)
    {
        Pool pool(fakeOptions(3));
        std::vector<unsigned long> pids;
        {
            std::vector<Pool::Lease> leases;
            for (int i = 0; i < 3; i++)
            {
                leases.push_back(pool.acquire());
                pids.push_back(leases.back().pid());
                leases.back().exchange("echo x", soon());
                leases.back().keep();
            }
        }
        ASSERT_EQ(pool.idleHelpers(), 3u);

        EXPECT_EQ(pool.shutdown(), 3u);
        EXPECT_EQ(pool.idleHelpers(), 0u);
        for (const auto pid : pids)
            EXPECT_FALSE(processAlive(pid));
        EXPECT_EQ(ask(pool, "echo again"), "again");
    }

    TEST(HelperPool, aHelperHeldDuringShutdownIsClosedWhenItIsReturned)
    {
        Pool pool(fakeOptions(1));
        std::optional<Pool::Lease> lease = pool.acquire();
        const unsigned long pid = lease->pid();
        lease->exchange("echo x", soon());
        lease->keep();

        EXPECT_EQ(pool.shutdown(), 0u) << "the only helper is held: it is closed when returned";
        lease.reset();
        EXPECT_EQ(pool.idleHelpers(), 0u);
        EXPECT_FALSE(processAlive(pid));
    }

    TEST(HelperPool, shutdownKillsAHelperThatDoesNotEndWithItsInput)
    {
        EnvVarGuard stay("DDB_FAKE_LINE_STAY_ON_EOF", "1");
        PoolOptions options = fakeOptions(1);
        options.closeTimeout = milliseconds(200);
        Pool pool(options);
        ask(pool, "echo x");
        const auto pids = pool.idleHelperPids();
        ASSERT_EQ(pids.size(), 1u);

        EXPECT_EQ(pool.shutdown(), 1u);
        EXPECT_FALSE(processAlive(pids[0]));
    }

    TEST(HelperPool, closeKillsTheHelpersAndLaterLeasesThrow)
    {
        Pool pool(fakeOptions(2));
        ask(pool, "echo x");
        const auto pids = pool.idleHelperPids();
        ASSERT_EQ(pids.size(), 1u);

        EXPECT_EQ(pool.close(), 1u);
        EXPECT_FALSE(processAlive(pids[0]));
        EXPECT_THROW(pool.acquire(), HelperUnavailableException);
        EXPECT_THROW(pool.tryAcquire(), HelperUnavailableException);
        EXPECT_EQ(pool.startedHelpers(), 1u) << "no helper was started after the close";
    }

    TEST(HelperPool, closeEndsTheWaitOfQueuedCallersAndRetiresHelpersInUse)
    {
        Pool pool(fakeOptions(1));
        std::optional<Pool::Lease> holder = pool.acquire();
        const unsigned long pid = holder->pid();
        holder->exchange("echo x", soon());
        holder->keep();

        std::atomic<bool> refused{false};
        Threads threads;
        threads.start([&] {
            try
            {
                pool.acquire();
            }
            catch (const HelperUnavailableException &)
            {
                refused = true;
            }
        });
        ASSERT_TRUE(pool.awaitWaiters(1, kPatience));
        pool.close();
        threads.join();
        EXPECT_TRUE(refused) << "a caller in the queue does not wait for a pool that is closed";
        EXPECT_EQ(pool.queuedCallers(), 0u);

        holder.reset();
        EXPECT_EQ(pool.idleHelpers(), 0u) << "a closed pool keeps no helper";
        EXPECT_FALSE(processAlive(pid));
    }

    // ------------------------------------------------------------------ how helpers start

    TEST(HelperPool, theEnvironmentOverridesReachTheHelper)
    {
        EnvVarGuard inherited("HELPER_POOL_TEST_INHERITED", "from the parent");
        PoolOptions options = fakeOptions(1);
        options.session.envOverrides = {{"HELPER_POOL_TEST_OVERRIDE", "set by the pool"}};
        Pool pool(options);
        EXPECT_EQ(ask(pool, "env HELPER_POOL_TEST_OVERRIDE"), "set by the pool");
        EXPECT_EQ(ask(pool, "env HELPER_POOL_TEST_INHERITED"), "from the parent");
        EXPECT_EQ(ask(pool, "env HELPER_POOL_TEST_NOT_SET"), "unset");
    }

    TEST(HelperPool, theMemoryLimitReachesTheHelper)
    {
#ifdef _WIN32
        GTEST_SKIP() << "the fake reports an address space limit only where the system has one per process";
#else
        SKIP_UNDER_SANITIZER();
        constexpr uint64_t kLimit = 512ull * 1024 * 1024;
        PoolOptions options = fakeOptions(1);
        options.session.maxMemoryBytes = kLimit;
        Pool limited(options);
        EXPECT_EQ(ask(limited, "rlimit"), std::to_string(kLimit));

        Pool unlimited(fakeOptions(1));
        EXPECT_NE(ask(unlimited, "rlimit"), std::to_string(kLimit));
#endif
    }

    // ------------------------------------------------------------------ the batch

    struct SomeOtherHelperException : HelperUnavailableException
    {
        using HelperUnavailableException::HelperUnavailableException;
    };

    TEST(HelperPool, aHelperThatCannotBeUsedStopsTheBatchWhateverItsKind)
    {
        static_assert(std::is_base_of_v<HelperUnavailableException, MetadataHelperException>);

        // An item error has a code; a helper that is missing or unusable has none, so the
        // exception goes on and the batch stops
        EXPECT_STREQ(addErrorCode(IndexException("x")), "INDEX");
        EXPECT_EQ(addErrorCode(HelperUnavailableException("x")), nullptr);
        EXPECT_EQ(addErrorCode(MetadataHelperException("x")), nullptr);
        EXPECT_EQ(addErrorCode(SomeOtherHelperException("x")), nullptr);

        // Also when the exception is passed on as the base class a handler catches
        try
        {
            throw MetadataHelperException("x");
        }
        catch (const AppException &e)
        {
            EXPECT_EQ(addErrorCode(e), nullptr);
        }
    }
} // namespace
