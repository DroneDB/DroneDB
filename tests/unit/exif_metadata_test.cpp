/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The pool of metadata-util processes behind the metadata reader (exif_helper_pool.h): lazy start, the
// limit, first come first served, idle closing with an injected clock, the way each kind of broken
// helper is handled, a missing or incompatible program, shutdown, and the real metadata-util with
// many threads. The failure modes run against ddbtest_fake_exif (tests/helpers), a compiled
// program, so they run on every platform; the real program is the one built next to the test
// binary. Waits are on conditions the pool or a process reports, never sleeps used to let a
// race pass.

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "../../vendor/base64/base64.h"
#include "cancellation.h"
#include "dbops.h"
#include "exceptions.h"
#include "exif_helper_pool.h"
#include "fs.h"
#include "gtest/gtest.h"
#include "log_forwarding.h"
#include "mio.h"
#include "process_init.h"
#include "process_runner.h"
#include "testarea.h"
#include "tool_locator.h"
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
    using exif::HelperMetadataReader;
    using std::chrono::milliseconds;
    using std::chrono::seconds;

    // How long a step that must succeed may take before the test calls it hung
    constexpr seconds kPatience(30);

    // A JPEG of 8 x 8 pixels without metadata (159 bytes)
    constexpr unsigned char kTinyJpeg[] = {
        0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01,
        0x00, 0x01, 0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43, 0x00, 0x10, 0x0B, 0x0C, 0x0E, 0x0C, 0x0A, 0x10,
        0x0E, 0x0D, 0x0E, 0x12, 0x11, 0x10, 0x13, 0x18, 0x28, 0x1A, 0x18, 0x16, 0x16, 0x18, 0x31, 0x23,
        0x25, 0x1D, 0x28, 0x3A, 0x33, 0x3D, 0x3C, 0x39, 0x33, 0x38, 0x37, 0x40, 0x48, 0x5C, 0x4E, 0x40,
        0x44, 0x57, 0x45, 0x37, 0x38, 0x50, 0x6D, 0x51, 0x57, 0x5F, 0x62, 0x67, 0x68, 0x67, 0x3E, 0x4D,
        0x71, 0x79, 0x70, 0x64, 0x78, 0x5C, 0x65, 0x67, 0x63, 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x08,
        0x00, 0x08, 0x01, 0x01, 0x11, 0x00, 0xFF, 0xC4, 0x00, 0x14, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xC4, 0x00, 0x14,
        0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00, 0x3F, 0xFF, 0xD9
    };

    fs::path fakeHelperPath()
    {
        return io::getExeFolderPath() / DDB_FAKE_EXIF_NAME;
    }

    fs::path realHelperPath()
    {
        return tools::findTool(exif::kMetadataUtilName, exif::kMetadataUtilEnv);
    }

    void writeTinyJpeg(const fs::path &file)
    {
        std::ofstream out(file, std::ios::binary);
        out.write(reinterpret_cast<const char *>(kTinyJpeg), sizeof(kTinyJpeg));
    }

    void touch(const fs::path &file)
    {
        std::ofstream(file, std::ios::binary) << "x";
    }

    // The options of a pool of the fake helper with room for `limit` helpers
    HelperMetadataReader::Options fakeOptions(size_t limit)
    {
        HelperMetadataReader::Options options;
        options.locate = [] { return fakeHelperPath(); };
        options.limit = [limit] { return limit; };
        return options;
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

    // Waits until `done()`; a condition of another process (a file it writes), so it looks again
    // at short intervals
    template <typename Done>
    bool waitFor(Done done, seconds patience = kPatience)
    {
        const auto deadline = std::chrono::steady_clock::now() + patience;
        while (!done())
        {
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            std::this_thread::sleep_for(milliseconds(2));
        }
        return true;
    }

    // The requests the fake helper received, one "<pid> <file name>" per line
    struct FakeRequest
    {
        unsigned long pid;
        std::string name;
    };

    std::vector<FakeRequest> fakeRequests(const fs::path &log)
    {
        std::vector<FakeRequest> requests;
        std::ifstream in(log);
        std::string line;
        while (std::getline(in, line))
        {
            const size_t space = line.find(' ');
            if (space != std::string::npos)
                requests.push_back({std::stoul(line.substr(0, space)), line.substr(space + 1)});
        }
        return requests;
    }

    bool waitForFakeRequests(const fs::path &log, size_t count)
    {
        return waitFor([&] { return fakeRequests(log).size() >= count; });
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

    // A request of the fake helper that stays open until the test opens the gate (or the scope
    // ends: declared after the Threads that wait for it, so it opens first)
    class Gate
    {
      public:
        explicit Gate(const fs::path &folder, const std::string &name) : request_(folder / (name + ".hold")) {}
        ~Gate() { open(); }
        // The file to read: the fake helper holds the request until the gate is open
        const fs::path &request() const { return request_; }
        void open() const { touch(fs::path(request_.string() + ".release")); }

      private:
        fs::path request_;
    };

    // The WARNING and ERROR records of the log while it lives. They come through the log
    // callback a host registers (log_forwarding.h), which the capture takes away again when it
    // ends: nothing of it outlives the test, as an appender added to plog would (plog cannot
    // remove one). One capture at a time.
    class WarningCapture
    {
      public:
        WarningCapture() { logfwd::setCallback(&collect, logfwd::kWarning, this); }
        // Returns once no thread is inside collect() any more
        ~WarningCapture() { logfwd::unregister(); }
        WarningCapture(const WarningCapture &) = delete;
        WarningCapture &operator=(const WarningCapture &) = delete;

        bool contains(const std::string &text) const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto &message : messages_)
                if (message.find(text) != std::string::npos)
                    return true;
            return false;
        }
        size_t count() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return messages_.size();
        }

      private:
        static void collect(int, const char *message, const char *, uint64_t, void *userData)
        {
            auto *capture = static_cast<WarningCapture *>(userData);
            std::lock_guard<std::mutex> lock(capture->mutex_);
            capture->messages_.emplace_back(message);
        }

        mutable std::mutex mutex_;
        std::vector<std::string> messages_;
    };

    // ------------------------------------------------------------------ the fake helper

    TEST(ExifHelperPool, startsNoHelperBeforeTheFirstRead)
    {
        HelperMetadataReader pool(fakeOptions(4));
        EXPECT_EQ(pool.startedHelpers(), 0u);
        EXPECT_EQ(pool.idleHelpers(), 0u);
        EXPECT_EQ(pool.busyHelpers(), 0u);

        const auto result = pool.read("a.jpg");
        ASSERT_TRUE(result.metadata.has_value());
        EXPECT_EQ(pool.startedHelpers(), 1u);
        EXPECT_EQ(pool.idleHelpers(), 1u);
        EXPECT_EQ(pool.busyHelpers(), 0u);
    }

    TEST(ExifHelperPool, aReadReturnsTheTagsOfTheResponse)
    {
        HelperMetadataReader pool(fakeOptions(1));
        const auto result = pool.read("photos/DJI_0001.JPG");
        ASSERT_TRUE(result.metadata.has_value());
        const auto &metadata = *result.metadata;
        EXPECT_EQ(metadata.width(), 4000);
        EXPECT_EQ(metadata.height(), 3000);
        ASSERT_NE(metadata.findExif("Exif.Image.Make"), nullptr);
        EXPECT_EQ(metadata.findExif("Exif.Image.Make")->toString(), "DJI");
        ASSERT_NE(metadata.findExif("Exif.Image.ImageDescription"), nullptr);
        EXPECT_EQ(metadata.findExif("Exif.Image.ImageDescription")->toString(),
                  io::toUtf8(fs::current_path() / "photos" / "DJI_0001.JPG"))
            << "the helper saw the path the caller gave, made absolute";
        ASSERT_NE(metadata.findXmp("Xmp.drone-dji.GimbalYawDegree"), nullptr);
        EXPECT_FLOAT_EQ(metadata.findXmp("Xmp.drone-dji.GimbalYawDegree")->toFloat(), 12.5F);
    }

    TEST(ExifHelperPool, oneHelperServesTheReadsOfOneCaller)
    {
        HelperMetadataReader pool(fakeOptions(4));
        for (int i = 0; i < 25; i++)
            ASSERT_TRUE(pool.read("f" + std::to_string(i) + ".jpg").metadata.has_value());
        EXPECT_EQ(pool.startedHelpers(), 1u);
        EXPECT_EQ(pool.requestsSent(), 25u);
    }

    TEST(ExifHelperPool, aFileWithoutMetadataIsAResultAndTheHelperStays)
    {
        HelperMetadataReader pool(fakeOptions(1));
        const auto notAnImage = pool.read("noimage.jpg");
        EXPECT_FALSE(notAnImage.metadata.has_value());
        EXPECT_EQ(notAnImage.message, "not an image");
        const auto missing = pool.read("missing.jpg");
        EXPECT_FALSE(missing.metadata.has_value());
        EXPECT_EQ(missing.message, "cannot open");
        EXPECT_TRUE(pool.read("good.jpg").metadata.has_value());
        EXPECT_EQ(pool.startedHelpers(), 1u) << "no helper was discarded";
    }

    TEST(ExifHelperPool, aBrokenHelperFailsTheFileAndTheNextReadGetsANewOne)
    {
        struct Case
        {
            const char *file;
            const char *reason; // a part of the log record
        };
        const Case cases[] = {{"crash.jpg", "exited with code 7"}, {"hang.jpg", "no response in"},
                              {"garbage.jpg", "invalid response"}, {"wrongid.jpg", "answered request 987654321"},
                              {"huge.jpg", "longer than"},         {"rejected.jpg", "rejected the request"}};
        for (const Case &broken : cases)
        {
            SCOPED_TRACE(broken.file);
            auto options = fakeOptions(1);
            options.requestTimeout = milliseconds(400);
            HelperMetadataReader pool(options);
            ASSERT_TRUE(pool.read("first.jpg").metadata.has_value());

            WarningCapture warnings;
            try
            {
                pool.read(broken.file);
                FAIL() << "the read should fail";
            }
            catch (const IndexException &e)
            {
                EXPECT_NE(std::string(e.what()).find(broken.file), std::string::npos) << e.what();
                EXPECT_NE(std::string(e.what()).find("metadata-util"), std::string::npos) << e.what();
            }
            EXPECT_TRUE(warnings.contains("Discarded metadata-util")) << "one WARNING per discarded helper";
            EXPECT_TRUE(warnings.contains(broken.file)) << "naming the file";
            EXPECT_TRUE(warnings.contains(broken.reason)) << "and the reason";

            // The helper is gone and its place is free: the next read starts another
            EXPECT_EQ(pool.idleHelpers(), 0u);
            EXPECT_EQ(pool.busyHelpers(), 0u);
            ASSERT_TRUE(pool.read("next.jpg").metadata.has_value());
            EXPECT_EQ(pool.startedHelpers(), 2u);
        }
    }

    TEST(ExifHelperPool, aBrokenHelperDoesNotAffectTheOthers)
    {
        TestArea area("ExifHelperPool.others", true);
        const fs::path log = area.getFolder() / "requests.log";
        EnvVarGuard logEnv("DDB_FAKE_EXIF_LOG", io::toUtf8(log));
        HelperMetadataReader pool(fakeOptions(2));

        Threads threads;
        Gate gate(area.getFolder(), "a");
        std::atomic<bool> aRead{false};
        threads.start([&] { aRead = pool.read(gate.request()).metadata.has_value(); });
        ASSERT_TRUE(waitForFakeRequests(log, 1)) << "helper A holds its request";
        const unsigned long pidA = fakeRequests(log).at(0).pid;

        EXPECT_THROW(pool.read("crash.jpg"), IndexException);

        gate.open();
        threads.join();
        EXPECT_TRUE(aRead) << "the helper that was busy answered normally";
        EXPECT_TRUE(processAlive(pidA));
        EXPECT_EQ(pool.idleHelperPids(), std::vector<unsigned long>{pidA});

        ASSERT_TRUE(pool.read("again.jpg").metadata.has_value());
        EXPECT_EQ(fakeRequests(log).back().pid, pidA) << "the next read goes to the helper that never failed";
        EXPECT_EQ(pool.startedHelpers(), 2u);
    }

    TEST(ExifHelperPool, callersAreServedInTheOrderTheyCame)
    {
        TestArea area("ExifHelperPool.fifo", true);
        const fs::path log = area.getFolder() / "requests.log";
        EnvVarGuard logEnv("DDB_FAKE_EXIF_LOG", io::toUtf8(log));
        HelperMetadataReader pool(fakeOptions(1));

        Threads threads;
        Gate gate(area.getFolder(), "gate");
        threads.start([&] { pool.read(gate.request()); });
        ASSERT_TRUE(waitForFakeRequests(log, 1));

        constexpr size_t callers = 8;
        for (size_t i = 0; i < callers; i++)
        {
            threads.start([&, i] { pool.read(area.getFolder() / ("w" + std::to_string(i) + ".jpg")); });
            // The caller is in the queue before the next one starts
            ASSERT_TRUE(pool.awaitWaiters(i + 1, kPatience));
        }
        EXPECT_EQ(pool.startedHelpers(), 1u) << "the pool is full: the callers wait";

        gate.open();
        threads.join();

        const auto requests = fakeRequests(log);
        ASSERT_EQ(requests.size(), callers + 1);
        for (size_t i = 0; i < callers; i++)
            EXPECT_EQ(requests[i + 1].name, "w" + std::to_string(i) + ".jpg") << "position " << i;
        EXPECT_EQ(pool.startedHelpers(), 1u);
        EXPECT_EQ(pool.queuedCallers(), 0u);
    }

    TEST(ExifHelperPool, theLimitIsReadWhenACallerTakesAHelper)
    {
        TestArea area("ExifHelperPool.limit", true);
        const fs::path log = area.getFolder() / "requests.log";
        EnvVarGuard logEnv("DDB_FAKE_EXIF_LOG", io::toUtf8(log));
        std::atomic<size_t> limit{1};
        auto options = fakeOptions(1);
        options.limit = [&limit] { return limit.load(); };
        HelperMetadataReader pool(options);

        Threads threads;
        Gate gate(area.getFolder(), "gate");
        threads.start([&] { pool.read(gate.request()); });
        ASSERT_TRUE(waitForFakeRequests(log, 1));
        std::atomic<bool> waiterRead{false};
        threads.start([&] { waiterRead = pool.read("waiter.jpg").metadata.has_value(); });
        ASSERT_TRUE(pool.awaitWaiters(1, kPatience));
        EXPECT_EQ(pool.startedHelpers(), 1u);

        // DDBConfigure made room: the waiter starts a second helper without waiting for the first
        limit = 2;
        EXPECT_TRUE(waitFor([&] { return waiterRead.load(); })) << "the waiter should have started a helper";
        EXPECT_EQ(pool.startedHelpers(), 2u);
        gate.open();
        threads.join();
    }

    TEST(ExifHelperPool, aHelperIdleForMoreThanAMinuteIsClosedWhenTheNextOneIsTaken)
    {
        std::atomic<int64_t> offsetSeconds{0};
        auto options = fakeOptions(1);
        options.now = [&] { return std::chrono::steady_clock::now() + seconds(offsetSeconds.load()); };
        HelperMetadataReader pool(options);

        ASSERT_TRUE(pool.read("a.jpg").metadata.has_value());
        const auto first = pool.idleHelperPids();
        ASSERT_EQ(first.size(), 1u);

        offsetSeconds = 59;
        ASSERT_TRUE(pool.read("b.jpg").metadata.has_value());
        EXPECT_EQ(pool.startedHelpers(), 1u) << "59 s idle: the helper is reused";
        EXPECT_EQ(pool.idleHelperPids(), first);

        offsetSeconds = 59 + 61;
        ASSERT_TRUE(pool.read("c.jpg").metadata.has_value());
        EXPECT_EQ(pool.startedHelpers(), 2u) << "61 s idle: the helper was closed and a new one started";
        EXPECT_FALSE(processAlive(first[0]));
        EXPECT_NE(pool.idleHelperPids(), first);
    }

    TEST(ExifHelperPool, anIdleHelperIsClosedWhenAnotherOneIsReturned)
    {
        TestArea area("ExifHelperPool.idleReturn", true);
        const fs::path log = area.getFolder() / "requests.log";
        EnvVarGuard logEnv("DDB_FAKE_EXIF_LOG", io::toUtf8(log));
        std::atomic<int64_t> offsetSeconds{0};
        auto options = fakeOptions(2);
        options.now = [&] { return std::chrono::steady_clock::now() + seconds(offsetSeconds.load()); };
        HelperMetadataReader pool(options);

        Threads threads;
        Gate gate(area.getFolder(), "busy");
        threads.start([&] { pool.read(gate.request()); });
        ASSERT_TRUE(waitForFakeRequests(log, 1));
        ASSERT_TRUE(pool.read("other.jpg").metadata.has_value());
        const auto idle = pool.idleHelperPids();
        ASSERT_EQ(idle.size(), 1u);

        offsetSeconds = 61;
        gate.open();
        threads.join();

        // The helper that was returned goes idle; the one that had been idle for 61 s is gone
        EXPECT_FALSE(processAlive(idle[0]));
        EXPECT_EQ(pool.idleHelpers(), 1u);
        EXPECT_NE(pool.idleHelperPids(), idle);
    }

    TEST(ExifHelperPool, idleHelpersOfAnotherProgramAreClosed)
    {
        TestArea area("ExifHelperPool.program", true);
        const fs::path copy = area.getFolder() / ("copy-" + fakeHelperPath().filename().string());
        fs::copy_file(fakeHelperPath(), copy, fs::copy_options::overwrite_existing);

        std::atomic<bool> useCopy{false};
        auto options = fakeOptions(1);
        options.locate = [&] { return useCopy ? copy : fakeHelperPath(); };
        HelperMetadataReader pool(options);

        ASSERT_TRUE(pool.read("a.jpg").metadata.has_value());
        const auto first = pool.idleHelperPids();
        useCopy = true;
        ASSERT_TRUE(pool.read("b.jpg").metadata.has_value());
        EXPECT_EQ(pool.startedHelpers(), 2u);
        EXPECT_FALSE(processAlive(first.at(0)));
        EXPECT_EQ(pool.idleHelpers(), 1u);
    }

    TEST(ExifHelperPool, aMissingProgramStopsTheBatchAndNamesTheVariable)
    {
        TestArea area("ExifHelperPool.missing", true);
        EnvVarGuard program(exif::kMetadataUtilEnv, io::toUtf8(area.getFolder() / "no-such-metadata-util"));
        HelperMetadataReader::Options options;
        options.limit = [] { return 1; };
        HelperMetadataReader pool(options);

        try
        {
            pool.read("a.jpg");
            FAIL() << "there is no program";
        }
        catch (const MetadataHelperException &e)
        {
            const std::string message = e.what();
            EXPECT_NE(message.find("metadata-util"), std::string::npos) << message;
            EXPECT_NE(message.find("DDB_METADATA_UTIL_PATH"), std::string::npos) << message;
            // Not an error of the file: ddb add stops instead of recording an item error
            EXPECT_EQ(addErrorCode(e), nullptr);
        }
        EXPECT_EQ(pool.startedHelpers(), 0u);
        EXPECT_EQ(pool.busyHelpers(), 0u);

        // The variable fixed, the next read works with the same pool
        program.restore();
        EnvVarGuard fake(exif::kMetadataUtilEnv, io::toUtf8(fakeHelperPath()));
        EXPECT_TRUE(pool.read("a.jpg").metadata.has_value());
    }

#ifndef _WIN32
    TEST(ExifHelperPool, aProgramThatIsNotExecutableIsMissing)
    {
        EnvVarGuard program(exif::kMetadataUtilEnv, "/dev/null");
        HelperMetadataReader::Options options;
        options.limit = [] { return 1; };
        HelperMetadataReader pool(options);
        EXPECT_THROW(pool.read("a.jpg"), MetadataHelperException);
        EXPECT_EQ(pool.busyHelpers(), 0u);
    }
#endif

    TEST(ExifHelperPool, aProgramThatDoesNotStartStopsTheBatchAndKeepsThePlace)
    {
        TestArea area("ExifHelperPool.notAProgram", true);
        const fs::path notAProgram = area.getFolder() / "not-a-program";
        std::ofstream(notAProgram) << "this is a text file";
#ifndef _WIN32
        fs::permissions(notAProgram, fs::perms::owner_all, fs::perm_options::add);
#endif
        std::atomic<bool> broken{true};
        auto options = fakeOptions(1);
        options.locate = [&] { return broken ? notAProgram : fakeHelperPath(); };
        HelperMetadataReader pool(options);

        try
        {
            pool.read("a.jpg");
            FAIL() << "the program cannot run";
        }
        catch (const MetadataHelperException &e)
        {
            EXPECT_NE(std::string(e.what()).find(io::toUtf8(notAProgram)), std::string::npos) << e.what();
        }
        EXPECT_EQ(pool.busyHelpers(), 0u) << "the place of the helper that never started is free";
        broken = false;
        EXPECT_TRUE(pool.read("a.jpg").metadata.has_value()) << "limit 1: a leaked place would hang here";
    }

    TEST(ExifHelperPool, aHelperOfAnotherProtocolMajorIsRefusedWithBothVersions)
    {
        auto options = fakeOptions(1);
        HelperMetadataReader pool(options);
        EnvVarGuard protocolVersion("DDB_FAKE_EXIF_PROTOCOL", "2.0");

        try
        {
            pool.read("a.jpg");
            FAIL() << "protocol 2.0 is not 1.x";
        }
        catch (const MetadataHelperException &e)
        {
            const std::string message = e.what();
            EXPECT_NE(message.find("protocol 2.0"), std::string::npos) << message;
            EXPECT_NE(message.find("1.0"), std::string::npos) << message;
            EXPECT_NE(message.find("metadata-util"), std::string::npos) << message;
            EXPECT_NE(message.find(io::toUtf8(fakeHelperPath())), std::string::npos) << message;
            EXPECT_EQ(addErrorCode(e), nullptr);
        }
        EXPECT_EQ(pool.startedHelpers(), 0u);
        EXPECT_EQ(pool.busyHelpers(), 0u);

        protocolVersion.restore();
        EXPECT_TRUE(pool.read("a.jpg").metadata.has_value());
    }

    size_t lineCount(const fs::path &file)
    {
        std::ifstream in(file);
        std::string line;
        size_t count = 0;
        while (std::getline(in, line))
            count++;
        return count;
    }

    TEST(ExifHelperPool, theVersionIsAskedOnceForAProgramFileAndAgainWhenItChanges)
    {
        TestArea area("ExifHelperPool.versionOnce", true);
        const fs::path log = area.getFolder() / "requests.log";
        const fs::path versions = area.getFolder() / "versions.log";
        EnvVarGuard logEnv("DDB_FAKE_EXIF_LOG", io::toUtf8(log));
        EnvVarGuard versionLogEnv("DDB_FAKE_EXIF_VERSION_LOG", io::toUtf8(versions));
        // A copy of the program, so the test can change the file
        const fs::path program = area.getFolder() / fakeHelperPath().filename();
        fs::copy_file(fakeHelperPath(), program);
        fs::permissions(program, fs::status(fakeHelperPath()).permissions());
        auto options = fakeOptions(3);
        options.locate = [&] { return program; };
        HelperMetadataReader pool(options);

        // Three helpers started together: one check
        {
            Threads threads;
            Gate first(area.getFolder(), "one");
            Gate second(area.getFolder(), "two");
            Gate third(area.getFolder(), "three");
            threads.start([&] { pool.read(first.request()); });
            threads.start([&] { pool.read(second.request()); });
            threads.start([&] { pool.read(third.request()); });
            ASSERT_TRUE(waitForFakeRequests(log, 3));
        }
        EXPECT_EQ(pool.startedHelpers(), 3u);
        EXPECT_EQ(lineCount(versions), 1u) << "the helpers of one program file share one check";

        // Started again later, the same file is not asked again
        pool.shutdown();
        ASSERT_TRUE(pool.read("a.jpg").metadata.has_value());
        EXPECT_EQ(pool.startedHelpers(), 4u);
        EXPECT_EQ(lineCount(versions), 1u);

        // The file changes (a newer time): asked again, and a refusal still stops the batch
        EnvVarGuard protocolVersion("DDB_FAKE_EXIF_PROTOCOL", "2.0");
        fs::last_write_time(program, fs::last_write_time(program) + std::chrono::hours(1));
        pool.shutdown();
        EXPECT_THROW(pool.read("a.jpg"), MetadataHelperException);
        EXPECT_EQ(lineCount(versions), 2u);

        // A refusal is not kept: once the program answers well, it is accepted
        protocolVersion.restore();
        ASSERT_TRUE(pool.read("a.jpg").metadata.has_value());
        EXPECT_EQ(lineCount(versions), 3u);
        EXPECT_EQ(pool.startedHelpers(), 5u);
    }

    TEST(ExifHelperPool, startsThatComeWhileTheFirstVersionCheckRunsWaitForItsAnswer)
    {
        // The check takes a second: the other starts come while it runs, and must take its answer
        // instead of asking the program themselves (a fast check would end before they came,
        // and they would find the answer whether they waited for it or not)
        TestArea area("ExifHelperPool.versionInFlight", true);
        const fs::path log = area.getFolder() / "requests.log";
        const fs::path versions = area.getFolder() / "versions.log";
        EnvVarGuard logEnv("DDB_FAKE_EXIF_LOG", io::toUtf8(log));
        EnvVarGuard versionLogEnv("DDB_FAKE_EXIF_VERSION_LOG", io::toUtf8(versions));
        EnvVarGuard slowVersion("DDB_FAKE_EXIF_VERSION_DELAY_MS", "1000");
        HelperMetadataReader pool(fakeOptions(3));
        {
            Threads threads;
            Gate first(area.getFolder(), "one");
            Gate second(area.getFolder(), "two");
            Gate third(area.getFolder(), "three");
            threads.start([&] { pool.read(first.request()); });
            threads.start([&] { pool.read(second.request()); });
            threads.start([&] { pool.read(third.request()); });
            ASSERT_TRUE(waitForFakeRequests(log, 3));
        }
        EXPECT_EQ(pool.startedHelpers(), 3u);
        EXPECT_EQ(lineCount(versions), 1u) << "the starts during the check waited for it";
    }

    TEST(ExifHelperPool, aHelperOfALowerMinorIsRefused)
    {
        if (exif::protocol::kMinorVersion == 0)
            GTEST_SKIP() << "the client implements minor 0: no lower minor exists";
        EnvVarGuard protocolVersion("DDB_FAKE_EXIF_PROTOCOL",
                                    std::to_string(exif::protocol::kMajorVersion) + "." +
                                        std::to_string(exif::protocol::kMinorVersion - 1));
        HelperMetadataReader pool(fakeOptions(1));
        EXPECT_THROW(pool.read("a.jpg"), MetadataHelperException);
    }

    TEST(ExifHelperPool, aCanceledStartGivesTheHelperPlaceBack)
    {
        HelperMetadataReader pool(fakeOptions(1));
        auto token = cancel::TokenRef::adopt(cancel::Token::create());
        token.get()->cancel("canceled by the test");
        {
            cancel::ScopedThreadToken scope(token);
            EXPECT_THROW(pool.read("a.jpg"), CanceledException);
        }
        EXPECT_EQ(pool.busyHelpers(), 0u);
        EXPECT_EQ(pool.idleHelpers(), 0u);
        EXPECT_TRUE(pool.read("a.jpg").metadata.has_value()) << "limit 1: a leaked place would hang here";
    }

    TEST(ExifHelperPool, aCanceledCallerLeavesTheQueue)
    {
        TestArea area("ExifHelperPool.canceledWaiter", true);
        const fs::path log = area.getFolder() / "requests.log";
        EnvVarGuard logEnv("DDB_FAKE_EXIF_LOG", io::toUtf8(log));
        HelperMetadataReader pool(fakeOptions(1));

        Threads threads;
        Gate gate(area.getFolder(), "gate");
        threads.start([&] { pool.read(gate.request()); });
        ASSERT_TRUE(waitForFakeRequests(log, 1));

        auto token = cancel::TokenRef::adopt(cancel::Token::create());
        std::atomic<bool> canceled{false};
        threads.start([&] {
            cancel::ScopedThreadToken scope(token);
            try
            {
                pool.read("waiter.jpg");
            }
            catch (const CanceledException &)
            {
                canceled = true;
            }
        });
        ASSERT_TRUE(pool.awaitWaiters(1, kPatience));
        token.get()->cancel("canceled by the test");
        ASSERT_TRUE(waitFor([&] { return canceled.load(); }));
        EXPECT_EQ(pool.queuedCallers(), 0u);

        // The next caller is not stuck behind the one that left
        std::atomic<bool> nextRead{false};
        threads.start([&] { nextRead = pool.read("next.jpg").metadata.has_value(); });
        ASSERT_TRUE(pool.awaitWaiters(1, kPatience));
        gate.open();
        threads.join();
        EXPECT_TRUE(nextRead);
    }

    // ------------------------------------------------------------------ a read started ahead

    // The metadata of a read as a comparable text: size, then every tag with its text
    std::string describe(const exif::ReadResult &result)
    {
        if (!result.metadata)
            return "no metadata: " + result.message;
        std::ostringstream text;
        text << result.metadata->width() << "x" << result.metadata->height();
        for (const auto *tags : {&result.metadata->exif(), &result.metadata->xmp()})
            for (const auto &tag : *tags)
                text << "\n" << tag.key() << " " << tag.typeName() << " " << tag.count() << " " << tag.toString();
        return text.str();
    }

    TEST(ExifHelperPool, aPendingReadGivesWhatAReadGivesOnce)
    {
        HelperMetadataReader pool(fakeOptions(1));
        const auto read = pool.read("photos/DJI_0001.JPG");
        auto pending = pool.begin("photos/DJI_0001.JPG");
        ASSERT_TRUE(pending.valid());
        const auto taken = pending.get();
        ASSERT_TRUE(read.metadata.has_value());
        EXPECT_EQ(describe(taken), describe(read));

        EXPECT_FALSE(pending.valid());
        EXPECT_THROW(pending.get(), std::logic_error);
        EXPECT_EQ(describe(pool.begin("noimage.jpg").get()), describe(pool.read("noimage.jpg")));
        EXPECT_EQ(pool.startedHelpers(), 1u) << "the helper went back to the pool after each";
        EXPECT_EQ(pool.idleHelpers(), 1u);
        EXPECT_EQ(pool.busyHelpers(), 0u);
    }

    TEST(ExifHelperPool, aStartedReadIsSentAtOnceAndHoldsItsHelperUntilItIsTaken)
    {
        TestArea area("ExifHelperPool.started", true);
        const fs::path log = area.getFolder() / "requests.log";
        EnvVarGuard logEnv("DDB_FAKE_EXIF_LOG", io::toUtf8(log));
        HelperMetadataReader pool(fakeOptions(1));
        Gate gate(area.getFolder(), "gate");

        // The helper holds its answer until the gate opens, so begin() returns before there is
        // one: the request is with the helper while the caller does something else
        auto pending = pool.begin(gate.request());
        EXPECT_EQ(pool.requestsSent(), 1u);
        EXPECT_EQ(pool.busyHelpers(), 1u);
        ASSERT_TRUE(waitForFakeRequests(log, 1));

        gate.open();
        const auto result = pending.get();
        ASSERT_TRUE(result.metadata.has_value());
        EXPECT_EQ(pool.busyHelpers(), 0u);
        EXPECT_EQ(pool.idleHelpers(), 1u);
        EXPECT_EQ(pool.startedHelpers(), 1u);
    }

    TEST(ExifHelperPool, aCallerThatWorksLongerThanTheRequestTimeoutStillGetsItsResult)
    {
        TestArea area("ExifHelperPool.longWork", true);
        const fs::path log = area.getFolder() / "requests.log";
        EnvVarGuard logEnv("DDB_FAKE_EXIF_LOG", io::toUtf8(log));
        auto options = fakeOptions(1);
        options.requestTimeout = milliseconds(200);
        HelperMetadataReader pool(options);

        auto pending = pool.begin("a.jpg");
        ASSERT_TRUE(waitForFakeRequests(log, 1));
        // The work of the caller (hashing a large video) outlasts the timeout: that time is not
        // the helper's, which answered at once
        std::this_thread::sleep_for(milliseconds(500));
        EXPECT_TRUE(pending.get().metadata.has_value());
        EXPECT_EQ(pool.idleHelpers(), 1u);
        EXPECT_EQ(pool.startedHelpers(), 1u);
    }

    TEST(ExifHelperPool, aPendingReadDroppedBeforeItsResponseDiscardsItsHelperAndFreesItsPlace)
    {
        TestArea area("ExifHelperPool.dropped", true);
        const fs::path log = area.getFolder() / "requests.log";
        EnvVarGuard logEnv("DDB_FAKE_EXIF_LOG", io::toUtf8(log));
        HelperMetadataReader pool(fakeOptions(1));
        Gate gate(area.getFolder(), "gate");
        WarningCapture warnings;

        {
            auto pending = pool.begin(gate.request());
            ASSERT_TRUE(waitForFakeRequests(log, 1));
        }
        // Killed, not closed: closing would wait for the request the helper is stuck in
        const unsigned long pid = fakeRequests(log).front().pid;
        EXPECT_TRUE(waitFor([&] { return !processAlive(pid); }));
        EXPECT_EQ(pool.busyHelpers(), 0u);
        EXPECT_EQ(pool.idleHelpers(), 0u) << "a helper with an unread response is never reused";
        EXPECT_EQ(warnings.count(), 0u) << "the caller left; nothing went wrong with the helper";

        // With room for one helper, a held place would make this read wait forever, and a
        // reused helper would answer with the response of the dropped read
        ASSERT_EQ(pool.busyHelpers(), 0u);
        const auto next = pool.read("next.jpg");
        ASSERT_TRUE(next.metadata.has_value());
        ASSERT_NE(next.metadata->findExif("Exif.Image.ImageDescription"), nullptr);
        EXPECT_NE(next.metadata->findExif("Exif.Image.ImageDescription")->toString().find("next.jpg"),
                  std::string::npos);
        EXPECT_EQ(pool.startedHelpers(), 2u);

        // A read that was answered and dropped unread goes the same way
        {
            auto pending = pool.begin("answered.jpg");
            ASSERT_TRUE(waitForFakeRequests(log, 3));
        }
        EXPECT_EQ(pool.busyHelpers(), 0u);
        EXPECT_EQ(pool.idleHelpers(), 0u);
        EXPECT_TRUE(pool.read("last.jpg").metadata.has_value());
        EXPECT_EQ(pool.startedHelpers(), 3u);
    }

    TEST(ExifHelperPool, whatKeepsAReadFromStartingIsThrownWhenItIsTaken)
    {
        {
            // begin() does not throw it: the caller's work in between fails first, as before a read()
            HelperMetadataReader::Options options;
            options.locate = [] { return fs::path(); };
            HelperMetadataReader pool(options);
            auto pending = pool.begin("a.jpg");
            EXPECT_THROW(pending.get(), MetadataHelperException);
        }

        HelperMetadataReader pool(fakeOptions(1));
        auto token = cancel::TokenRef::adopt(cancel::Token::create());
        token.get()->cancel("canceled by the test");
        {
            cancel::ScopedThreadToken scope(token);
            auto pending = pool.begin("a.jpg");
            EXPECT_THROW(pending.get(), CanceledException);
        }
        EXPECT_EQ(pool.busyHelpers(), 0u);
        EXPECT_TRUE(pool.read("a.jpg").metadata.has_value()) << "limit 1: a leaked place would hang here";
    }

    TEST(ExifHelperPool, aReadStartedWhileEveryHelperIsBusyWaitsForNoneAndIsMadeWhenItIsTaken)
    {
        TestArea area("ExifHelperPool.startedWhenBusy", true);
        const fs::path log = area.getFolder() / "requests.log";
        EnvVarGuard logEnv("DDB_FAKE_EXIF_LOG", io::toUtf8(log));
        HelperMetadataReader pool(fakeOptions(1));

        // What the other thread saw right after its begin() returned
        std::atomic<bool> begun{false};
        std::atomic<size_t> sentWhenBegun{0};
        std::atomic<size_t> queuedWhenBegun{0};
        std::string secondResult;

        Threads threads;
        // Destroyed before the threads are joined, so a begin() that waits cannot keep the test
        // from ending: the held read goes (its helper is killed) and the promise is broken
        std::promise<void> firstTaken;
        Gate gate(area.getFolder(), "gate");
        // The only helper is held by a read started ahead, whose response the gate holds back
        auto first = pool.begin(gate.request());
        ASSERT_TRUE(waitForFakeRequests(log, 1));

        threads.start([&, taken = firstTaken.get_future()]() mutable {
            // A caller about to hash a large file: it must not wait for a helper to begin
            auto second = pool.begin(area.getFolder() / "second.jpg");
            sentWhenBegun = pool.requestsSent();
            queuedWhenBegun = pool.queuedCallers();
            begun = true;
            taken.wait();
            secondResult = describe(second.get());
        });
        ASSERT_TRUE(waitFor([&] { return begun.load(); })) << "begin() waited for the helper the first read holds";
        EXPECT_EQ(sentWhenBegun.load(), 1u) << "no helper was free: nothing is sent ahead";
        EXPECT_EQ(queuedWhenBegun.load(), 0u) << "begin() does not queue";
        EXPECT_EQ(pool.busyHelpers(), 1u);

        gate.open();
        EXPECT_TRUE(first.get().metadata.has_value());
        firstTaken.set_value();
        threads.join();

        EXPECT_NE(secondResult.find("second.jpg"), std::string::npos) << secondResult;
        const auto requests = fakeRequests(log);
        ASSERT_EQ(requests.size(), 2u);
        EXPECT_EQ(requests[1].name, "second.jpg");
        EXPECT_EQ(pool.startedHelpers(), 1u) << "the second read took the helper the first gave back";
        EXPECT_EQ(pool.busyHelpers(), 0u);
        EXPECT_EQ(pool.idleHelpers(), 1u);
    }

    TEST(ExifHelperPool, aReadStartedWhileCallersAreQueuedDoesNotPassThem)
    {
        TestArea area("ExifHelperPool.startedBehindQueue", true);
        const fs::path log = area.getFolder() / "requests.log";
        EnvVarGuard logEnv("DDB_FAKE_EXIF_LOG", io::toUtf8(log));
        // The thread that calls begin() is told the pool has room for a second helper, every
        // other thread that it is full: the queued caller keeps waiting, and only the queue
        // stands between begin() and that room
        std::atomic<std::thread::id> beginner{};
        auto options = fakeOptions(1);
        options.limit = [&beginner] { return std::this_thread::get_id() == beginner.load() ? size_t(2) : size_t(1); };
        // The held read outlasts the wait for begin(): a begin() that waits fails the test
        // there, not by the end of the held read
        options.requestTimeout = 4 * kPatience;
        HelperMetadataReader pool(options);

        std::atomic<bool> waiterServed{false};
        std::atomic<bool> begun{false};
        std::atomic<size_t> sentWhenBegun{0};
        std::atomic<size_t> startedWhenBegun{0};
        std::atomic<bool> lateRead{false};

        Threads threads;
        // Destroyed before the threads are joined (see the test above)
        std::promise<void> waiterDone;
        Gate gate(area.getFolder(), "gate");
        threads.start([&] { pool.read(gate.request()); });
        ASSERT_TRUE(waitForFakeRequests(log, 1));
        threads.start([&] { waiterServed = pool.read(area.getFolder() / "waiter.jpg").metadata.has_value(); });
        ASSERT_TRUE(pool.awaitWaiters(1, kPatience));

        threads.start([&, done = waiterDone.get_future()]() mutable {
            beginner = std::this_thread::get_id();
            auto pending = pool.begin(area.getFolder() / "late.jpg");
            sentWhenBegun = pool.requestsSent();
            startedWhenBegun = pool.startedHelpers();
            begun = true;
            done.wait();
            lateRead = pending.get().metadata.has_value();
        });
        ASSERT_TRUE(waitFor([&] { return begun.load(); })) << "begin() waited";
        EXPECT_EQ(sentWhenBegun.load(), 1u) << "the room was for the queued caller: begin() sent nothing";
        EXPECT_EQ(startedWhenBegun.load(), 1u);
        EXPECT_EQ(pool.queuedCallers(), 1u) << "the waiter is still first";

        gate.open();
        ASSERT_TRUE(waitFor([&] { return waiterServed.load(); }));
        waiterDone.set_value();
        threads.join();

        EXPECT_TRUE(lateRead.load());
        const auto requests = fakeRequests(log);
        ASSERT_EQ(requests.size(), 3u);
        EXPECT_EQ(requests[1].name, "waiter.jpg");
        EXPECT_EQ(requests[2].name, "late.jpg");
        EXPECT_EQ(pool.busyHelpers(), 0u);
        EXPECT_EQ(pool.queuedCallers(), 0u);
    }

    TEST(ExifHelperPool, shutdownClosesTheIdleHelpersAndThePoolWorksAgain)
    {
        TestArea area("ExifHelperPool.shutdown", true);
        const fs::path log = area.getFolder() / "requests.log";
        EnvVarGuard logEnv("DDB_FAKE_EXIF_LOG", io::toUtf8(log));
        HelperMetadataReader pool(fakeOptions(3));

        // Three helpers at once, then all idle
        Threads threads;
        Gate first(area.getFolder(), "one");
        Gate second(area.getFolder(), "two");
        Gate third(area.getFolder(), "three");
        threads.start([&] { pool.read(first.request()); });
        threads.start([&] { pool.read(second.request()); });
        threads.start([&] { pool.read(third.request()); });
        ASSERT_TRUE(waitForFakeRequests(log, 3));
        first.open();
        second.open();
        third.open();
        threads.join();
        const auto pids = pool.idleHelperPids();
        ASSERT_EQ(pids.size(), 3u);
        for (const auto pid : pids)
            ASSERT_TRUE(processAlive(pid));

        EXPECT_EQ(pool.shutdown(), 3u);
        for (const auto pid : pids)
            EXPECT_FALSE(processAlive(pid)) << "pid " << pid;
        EXPECT_EQ(pool.idleHelpers(), 0u);

        ASSERT_TRUE(pool.read("again.jpg").metadata.has_value());
        EXPECT_EQ(pool.startedHelpers(), 4u);
    }

    // The tests wait for and count the lines of the request log, which the helpers of a pool
    // append to at the same moment: no line may be lost. Two helpers that appended at once with
    // std::ofstream on Windows left one line over the other, and a test that started three helpers
    // together then waited for a third line that never came.
    TEST(ExifHelperPool, theRequestLogHasTheLineOfEveryRequestOfHelpersThatReadTogether)
    {
        TestArea area("ExifHelperPool.requestLog", true);
        const fs::path log = area.getFolder() / "requests.log";
        EnvVarGuard logEnv("DDB_FAKE_EXIF_LOG", io::toUtf8(log));
        constexpr size_t kHelpers = 8;
        constexpr size_t kReadsEach = 100;
        HelperMetadataReader pool(fakeOptions(kHelpers));
        std::atomic<size_t> served{0};
        {
            Threads threads;
            for (size_t t = 0; t < kHelpers; t++)
                threads.start([&, t] {
                    for (size_t i = 0; i < kReadsEach; i++)
                        if (pool.read(std::to_string(t) + "-" + std::to_string(i) + ".jpg").metadata.has_value())
                            served++;
                });
        }
        ASSERT_EQ(served.load(), kHelpers * kReadsEach);

        const auto requests = fakeRequests(log);
        std::set<std::string> names;
        for (const auto &request : requests)
            names.insert(request.name);
        EXPECT_EQ(requests.size(), kHelpers * kReadsEach);
        EXPECT_EQ(names.size(), kHelpers * kReadsEach) << "every request has its own whole line";
    }

    TEST(ExifHelperPool, aCallerWaitingDuringShutdownIsServed)
    {
        TestArea area("ExifHelperPool.shutdownWaiter", true);
        const fs::path log = area.getFolder() / "requests.log";
        EnvVarGuard logEnv("DDB_FAKE_EXIF_LOG", io::toUtf8(log));
        HelperMetadataReader pool(fakeOptions(1));

        Threads threads;
        Gate gate(area.getFolder(), "gate");
        std::atomic<bool> holderRead{false};
        std::atomic<bool> waiterRead{false};
        threads.start([&] { holderRead = pool.read(gate.request()).metadata.has_value(); });
        ASSERT_TRUE(waitForFakeRequests(log, 1));
        const unsigned long holderPid = fakeRequests(log).at(0).pid;
        threads.start([&] { waiterRead = pool.read("waiter.jpg").metadata.has_value(); });
        ASSERT_TRUE(pool.awaitWaiters(1, kPatience));

        EXPECT_EQ(pool.shutdown(), 0u) << "the only helper is busy: it is closed when it is returned";
        gate.open();
        threads.join();

        EXPECT_TRUE(holderRead);
        EXPECT_TRUE(waiterRead);
        EXPECT_FALSE(processAlive(holderPid)) << "the helper of before the shutdown was closed on return";
        EXPECT_EQ(pool.startedHelpers(), 2u);
        EXPECT_EQ(pool.idleHelpers(), 1u);
    }

    TEST(ExifHelperPool, shutdownKillsAHelperThatDoesNotEndWithItsInput)
    {
        EnvVarGuard stubborn("DDB_FAKE_EXIF_EXIT_ON_EOF_DELAY_MS", "20000");
        auto options = fakeOptions(1);
        options.closeTimeout = milliseconds(300);
        HelperMetadataReader pool(options);
        ASSERT_TRUE(pool.read("a.jpg").metadata.has_value());
        const auto pids = pool.idleHelperPids();
        ASSERT_EQ(pids.size(), 1u);

        WarningCapture warnings;
        const auto begin = std::chrono::steady_clock::now();
        EXPECT_EQ(pool.shutdown(), 1u);
        EXPECT_LT(std::chrono::steady_clock::now() - begin, seconds(10)) << "the wait is bounded by the close timeout";
        EXPECT_FALSE(processAlive(pids[0]));
        EXPECT_TRUE(warnings.contains("killed"));
    }

    TEST(ExifHelperPool, anIdleHelperThatDiedIsReplacedAndTheReadSucceeds)
    {
        HelperMetadataReader pool(fakeOptions(1));
        ASSERT_TRUE(pool.read("a.jpg").metadata.has_value());
        const auto pids = pool.idleHelperPids();
        ASSERT_EQ(pids.size(), 1u);
        killProcess(pids[0]);
        ASSERT_TRUE(waitFor([&] { return !processAlive(pids[0]); }));

        WarningCapture warnings;
        exif::ReadResult result;
        ASSERT_NO_THROW(result = pool.read("b.jpg")) << "a helper that died while idle is not the file's fault";
        EXPECT_TRUE(result.metadata.has_value());
        EXPECT_EQ(pool.startedHelpers(), 2u) << "a new helper took the place of the dead one";
        EXPECT_EQ(pool.busyHelpers(), 0u);
        EXPECT_EQ(pool.idleHelpers(), 1u);
        EXPECT_TRUE(warnings.contains("pid " + std::to_string(pids[0]))) << "the dead helper is logged";
    }

    TEST(ExifHelperPool, aHelperThatEndsBadlyWhenClosedIsLoggedWithItsStatusAndStderr)
    {
        {
            HelperMetadataReader pool(fakeOptions(1));
            ASSERT_TRUE(pool.read("a.jpg").metadata.has_value());
            WarningCapture warnings;
            EXPECT_EQ(pool.shutdown(), 1u);
            EXPECT_EQ(warnings.count(), 0u) << "a helper that ends well is not worth a warning";
        }

        EnvVarGuard status("DDB_FAKE_EXIF_EXIT_STATUS", "23");
        HelperMetadataReader pool(fakeOptions(1));
        ASSERT_TRUE(pool.read("a.jpg").metadata.has_value());
        WarningCapture warnings;
        EXPECT_EQ(pool.shutdown(), 1u);
        EXPECT_TRUE(warnings.contains("exited with code 23")) << "the exit status is in the log";
        EXPECT_TRUE(warnings.contains("a leak report at exit")) << "and what the helper said on stderr";
    }

    TEST(ExifHelperPool, closingTheKeptResourcesClosesTheHelpersOfTheProcessReader)
    {
        EnvVarGuard program(exif::kMetadataUtilEnv, io::toUtf8(fakeHelperPath()));
        HelperMetadataReader &reader = HelperMetadataReader::instance();
        ASSERT_EQ(&exif::metadataReader(), &reader);

        ASSERT_TRUE(exif::metadataReader().read("a.jpg").metadata.has_value());
        const auto pids = reader.idleHelperPids();
        ASSERT_FALSE(pids.empty());

        closeKeptResources();
        for (const auto pid : pids)
            EXPECT_FALSE(processAlive(pid)) << "pid " << pid;
        EXPECT_EQ(reader.idleHelpers(), 0u);

        // Usable again, as the unit tests that restart after DDBShutdown need
        EXPECT_TRUE(exif::metadataReader().read("b.jpg").metadata.has_value());
        closeKeptResources();
    }

    TEST(ExifHelperPool, aWriteReturnsTheTagsThatWereSet)
    {
        HelperMetadataReader pool(fakeOptions(1));
        const auto result = pool.write("a.jpg", {{"Exif.Image.Make", "DJI"}});
        EXPECT_TRUE(result.ok) << result.message;
        ASSERT_EQ(result.tags.size(), 1u);
        EXPECT_EQ(result.tags[0].key(), "Exif.Image.Make");
    }

    TEST(ExifHelperPool, aWriteThatBreaksTheHelperIsAFailedResultNotAnException)
    {
        HelperMetadataReader pool(fakeOptions(1));
        WarningCapture warnings;
        const auto result = pool.write("crash.jpg", {{"Exif.Image.Make", "DJI"}});
        EXPECT_FALSE(result.ok);
        EXPECT_NE(result.message.find("metadata-util"), std::string::npos) << result.message;
        EXPECT_TRUE(warnings.contains("crash.jpg"));
        EXPECT_TRUE(pool.write("a.jpg", {{"Exif.Image.Make", "DJI"}}).ok);
        EXPECT_EQ(pool.startedHelpers(), 2u);
    }

    TEST(ExifHelperPool, aWriteOfAKeyThatIsNotTextNeverReachesAHelper)
    {
        HelperMetadataReader pool(fakeOptions(1));
        const auto result = pool.write("a.jpg", {{"Exif.\xFF", "x"}});
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(pool.startedHelpers(), 0u);
    }

    TEST(ExifHelperPool, aPathThatIsNotUtf8ReachesTheHelperAsBase64)
    {
#ifdef _WIN32
        GTEST_SKIP() << "a Windows path is always text";
#else
        HelperMetadataReader pool(fakeOptions(1));
        const std::string name = "dir/caf\xE9.jpg";
        const auto result = pool.read(fs::path(name));
        ASSERT_TRUE(result.metadata.has_value());
        EXPECT_EQ(result.metadata->findExif("Exif.Image.ImageDescription")->toString(),
                  "path64:" + Base64::encode((fs::current_path() / name).string()));
#endif
    }

    TEST(ExifHelperPool, aMetadataReaderOfATestReplacesTheProcessReaderWhileInScope)
    {
        struct Recorder : exif::IMetadataReader
        {
            exif::ReadResult read(const fs::path &) override { return {}; }
            exif::WriteResult write(const fs::path &, const exif::TagAssignments &) override { return {}; }
        };
        exif::IMetadataReader *const process = &exif::metadataReader();
        EXPECT_EQ(process, &HelperMetadataReader::instance());
        Recorder outer;
        Recorder inner;
        {
            exif::ScopedMetadataReader scopeOuter(outer);
            EXPECT_EQ(&exif::metadataReader(), &outer);
            {
                exif::ScopedMetadataReader scopeInner(inner);
                EXPECT_EQ(&exif::metadataReader(), &inner);
            }
            EXPECT_EQ(&exif::metadataReader(), &outer);
        }
        EXPECT_EQ(&exif::metadataReader(), process);
    }

    TEST(ExifHelperPool, aFailureOfTheHelperIsAnItemErrorAndAMissingHelperIsNot)
    {
        EXPECT_STREQ(addErrorCode(IndexException("x")), "INDEX");
        EXPECT_EQ(addErrorCode(MetadataHelperException("x")), nullptr);
    }

    // ------------------------------------------------------------------ the real metadata-util

    // A folder with a JPEG that has metadata, written through the real helper
    class ExifHelperPoolReal : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            ASSERT_FALSE(realHelperPath().empty()) << "metadata-util is built next to the test binary";
            area_ = std::make_unique<TestArea>(std::string("ExifHelperPool.real.") +
                                                   ::testing::UnitTest::GetInstance()->current_test_info()->name(),
                                               true);
            photo_ = area_->getFolder() / "photo.jpg";
            writeTinyJpeg(photo_);
            const auto written = pool_.write(photo_, {{"Exif.Image.Make", "DJI"},
                                                      {"Exif.Image.Model", "FC220"},
                                                      {"Exif.GPSInfo.GPSLatitude", "46/1 4/1 5129/1000"},
                                                      {"Exif.GPSInfo.GPSLatitudeRef", "N"}});
            ASSERT_TRUE(written.ok) << written.message;
        }

        std::unique_ptr<TestArea> area_;
        fs::path photo_;
        HelperMetadataReader pool_;
    };

    TEST_F(ExifHelperPoolReal, whatWasWrittenIsReadBack)
    {
        const auto result = pool_.read(photo_);
        ASSERT_TRUE(result.metadata.has_value()) << result.message;
        const auto &metadata = *result.metadata;
        EXPECT_EQ(metadata.width(), 8);
        EXPECT_EQ(metadata.height(), 8);
        ASSERT_NE(metadata.findExif("Exif.Image.Make"), nullptr);
        EXPECT_EQ(metadata.findExif("Exif.Image.Make")->toString(), "DJI");
        EXPECT_EQ(metadata.findExif("Exif.Image.Make")->typeName(), "Ascii");

        const exif::Tag *latitude = metadata.findExif("Exif.GPSInfo.GPSLatitude");
        ASSERT_NE(latitude, nullptr);
        EXPECT_EQ(latitude->typeName(), "Rational");
        EXPECT_EQ(latitude->count(), 3u);
        EXPECT_EQ(latitude->toRational(0), (exif::Rational{46, 1}));
        EXPECT_EQ(latitude->toRational(2), (exif::Rational{5129, 1000}));
        EXPECT_FLOAT_EQ(latitude->toFloat(2), 5.129F);
        EXPECT_THROW(latitude->toRational(3), std::out_of_range);
    }

    TEST_F(ExifHelperPoolReal, aValueThatIsNotUtf8SurvivesTheTrip)
    {
        const std::string latin1 = "Caf\xE9";
        const auto written = pool_.write(photo_, {{"Exif.Image.Make", latin1}});
        ASSERT_TRUE(written.ok) << written.message;
        ASSERT_EQ(written.tags.size(), 1u);
        EXPECT_EQ(written.tags[0].toString(), latin1);

        const auto result = pool_.read(photo_);
        ASSERT_TRUE(result.metadata.has_value());
        ASSERT_NE(result.metadata->findExif("Exif.Image.Make"), nullptr);
        EXPECT_EQ(result.metadata->findExif("Exif.Image.Make")->toString(), latin1) << "the raw bytes";
    }

    TEST_F(ExifHelperPoolReal, aFileThatIsNotAnImageHasNoMetadata)
    {
        const fs::path text = area_->getFolder() / "text.jpg";
        std::ofstream(text) << "not an image";
        const auto result = pool_.read(text);
        EXPECT_FALSE(result.metadata.has_value());
        EXPECT_FALSE(result.message.empty());

        const auto missing = pool_.read(area_->getFolder() / "missing.jpg");
        EXPECT_FALSE(missing.metadata.has_value());
        EXPECT_FALSE(missing.message.empty());

        EXPECT_TRUE(pool_.read(photo_).metadata.has_value());
        EXPECT_EQ(pool_.startedHelpers(), 1u) << "a file without metadata does not cost the helper";
    }

    TEST_F(ExifHelperPoolReal, aWriteThatFailsIsAResultWithTheReason)
    {
        const auto result = pool_.write(area_->getFolder() / "missing.jpg", {{"Exif.Image.Make", "x"}});
        EXPECT_FALSE(result.ok);
        EXPECT_FALSE(result.message.empty());
        const auto badKey = pool_.write(photo_, {{"Not.A.Key", "x"}});
        EXPECT_FALSE(badKey.ok);
        EXPECT_TRUE(pool_.read(photo_).metadata.has_value());
    }

    TEST_F(ExifHelperPoolReal, aFileNameThatIsNotUtf8IsReadThroughItsBytes)
    {
#ifdef _WIN32
        GTEST_SKIP() << "a Windows path is always text";
#else
        const fs::path odd = area_->getFolder() / fs::path(std::string("caf\xE9.jpg"));
        std::error_code ec;
        fs::copy_file(photo_, odd, ec);
        if (ec)
            GTEST_SKIP() << "this file system refuses a name that is not UTF-8: " << ec.message();
        const auto result = pool_.read(odd);
        ASSERT_TRUE(result.metadata.has_value()) << result.message;
        EXPECT_EQ(result.metadata->width(), 8);
        EXPECT_EQ(result.metadata->findExif("Exif.Image.Make")->toString(), "DJI");
#endif
    }

    TEST_F(ExifHelperPoolReal, manyThreadsShareTheLimitedHelpers)
    {
        ScopedProcessConfig config(R"({"cpuPoolSize":3})");
        HelperMetadataReader pool; // the default limit: cpuPoolSize + 1 = 4
        constexpr size_t threadCount = 64;
        constexpr size_t readsPerThread = 50;

        std::atomic<size_t> wrong{0};
        std::mutex mutex;
        std::string firstProblem;
        {
            Threads threads;
            for (size_t t = 0; t < threadCount; t++)
                threads.start([&] {
                    for (size_t i = 0; i < readsPerThread; i++)
                    {
                        try
                        {
                            const auto result = pool.read(photo_);
                            const exif::Tag *make = result.metadata ? result.metadata->findExif("Exif.Image.Make") : nullptr;
                            if (make == nullptr || make->toString() != "DJI" || result.metadata->width() != 8)
                                throw std::runtime_error("a read returned the wrong metadata");
                        }
                        catch (const std::exception &e)
                        {
                            if (wrong++ == 0)
                            {
                                std::lock_guard<std::mutex> lock(mutex);
                                firstProblem = e.what();
                            }
                        }
                    }
                });
        }
        EXPECT_EQ(wrong.load(), 0u) << firstProblem;
        EXPECT_LE(pool.startedHelpers(), 4u) << "never more helpers than the limit";
        EXPECT_EQ(pool.requestsSent(), threadCount * readsPerThread) << "one request per read";
        EXPECT_EQ(pool.busyHelpers(), 0u);
        EXPECT_EQ(pool.queuedCallers(), 0u);
        EXPECT_EQ(pool.shutdown(), pool.startedHelpers());
    }

    // The working directory of the process while it lives; restores the one it found
    class WorkingDirectory
    {
      public:
        WorkingDirectory() : saved_(fs::current_path()) {}
        ~WorkingDirectory()
        {
            std::error_code ec;
            fs::current_path(saved_, ec);
        }
        WorkingDirectory(const WorkingDirectory &) = delete;
        WorkingDirectory &operator=(const WorkingDirectory &) = delete;

      private:
        fs::path saved_;
    };

    TEST(ExifHelperPool, aRelativePathIsReadFromTheWorkingDirectoryTheCallerHasWhenItReads)
    {
        ASSERT_FALSE(realHelperPath().empty()) << "metadata-util is built next to the test binary";
        TestArea area("ExifHelperPool.relativePath", true);
        const fs::path first = area.getFolder() / "first";
        const fs::path second = area.getFolder() / "second";
        {
            HelperMetadataReader setup;
            for (const auto &[folder, make] : {std::pair(first, "First"), std::pair(second, "Second")})
            {
                fs::create_directories(folder);
                writeTinyJpeg(folder / "photo.jpg");
                const auto written = setup.write(folder / "photo.jpg", {{"Exif.Image.Make", make}});
                ASSERT_TRUE(written.ok) << written.message;
            }
        }

        const auto makeOf = [](const exif::ReadResult &result) {
            const exif::Tag *make = result.metadata ? result.metadata->findExif("Exif.Image.Make") : nullptr;
            return make != nullptr ? make->toString() : "(no metadata: " + result.message + ")";
        };

        // The helper starts in the first folder and keeps running when the host moves on, as a
        // host that changes its working directory between reads does
        HelperMetadataReader pool;
        WorkingDirectory restore;
        fs::current_path(first);
        EXPECT_EQ(makeOf(pool.read("photo.jpg")), "First");
        fs::current_path(second);
        EXPECT_EQ(makeOf(pool.read("photo.jpg")), "Second") << "the caller's folder, not the helper's";
        EXPECT_EQ(pool.startedHelpers(), 1u) << "the same helper read both";
    }

    // ------------------------------------------------------------------ a host that dies

#ifdef __linux__
    constexpr const char *kHostModeEnv = "DDB_TEST_EXIF_HOST_MODE";
    constexpr const char *kHostFolderEnv = "DDB_TEST_EXIF_HOST_FOLDER";

    // The host side, run in a process of its own by the test below (disabled: never part of a
    // normal run). It starts helpers through the reader of the process, leaves them idle, tells
    // its parent their ids and waits to be killed.
    TEST(ExifHelperHost, DISABLED_startsHelpersAndWaitsToBeKilled)
    {
        const char *mode = std::getenv(kHostModeEnv);
        const char *folderText = std::getenv(kHostFolderEnv);
        if (mode == nullptr || folderText == nullptr)
            GTEST_SKIP() << "Only run by ExifHelperHost.killedHostLeavesNoHelper";
        const fs::path folder = io::fromUtf8(folderText);
        const bool fake = std::string(mode) == "fake";
        exif::IMetadataReader &reader = exif::metadataReader();
        HelperMetadataReader &pool = HelperMetadataReader::instance();

        Threads threads;
        std::vector<std::unique_ptr<Gate>> gates;
        if (fake)
        {
            // Four helpers alive at once: each holds a request until its gate opens
            for (int i = 0; i < 4; i++)
            {
                gates.push_back(std::make_unique<Gate>(folder, "host" + std::to_string(i)));
                threads.start([&reader, path = gates.back()->request()] { reader.read(path); });
            }
            ASSERT_TRUE(waitForFakeRequests(folder / "requests.log", 4));
            for (const auto &gate : gates)
                gate->open();
        }
        else
        {
            for (int t = 0; t < 8; t++)
                threads.start([&reader, photo = folder / "photo.jpg"] {
                    for (int i = 0; i < 100; i++)
                        reader.read(photo);
                });
        }
        threads.join();

        std::string ready = "READY";
        for (const auto pid : pool.idleHelperPids())
            ready += " " + std::to_string(pid);
        std::cout << ready << std::endl;
        std::this_thread::sleep_for(seconds(120));
    }

    // Normal exit and a killed host are different: nothing runs in a host that was killed. The
    // helpers end because their input ends (nothing else holds the other end of their pipe), also
    // when they are idle in a pool that is never closed.
    void expectNoHelperSurvivesAKilledHost(const std::string &mode)
    {
        TestArea area("ExifHelperPool.host." + mode, true);
        const fs::path folder = area.getFolder();
        writeTinyJpeg(folder / "photo.jpg");

        process::SessionOptions options;
        options.envOverrides = {{kHostModeEnv, mode},
                                {kHostFolderEnv, io::toUtf8(folder)},
                                {"DDB_CPU_POOL_SIZE", "3"},
                                {"DDB_FAKE_EXIF_LOG", io::toUtf8(folder / "requests.log")},
                                {exif::kMetadataUtilEnv, io::toUtf8(mode == "fake" ? fakeHelperPath() : realHelperPath())}};
        process::Session host;
        ASSERT_TRUE(host.start(io::getExeFolderPath() / DDB_UNIT_TESTS_NAME,
                               {"--gtest_also_run_disabled_tests",
                                "--gtest_filter=ExifHelperHost.DISABLED_startsHelpersAndWaitsToBeKilled"},
                               options))
            << host.startError();

        // The log of the child is on stdout too: the line of the ids starts with READY
        std::vector<unsigned long> helpers;
        const auto deadline = process::Session::Clock::now() + seconds(90);
        for (;;)
        {
            const auto line = host.readLine(deadline, 1024 * 1024);
            ASSERT_EQ(line.status, process::Session::ReadStatus::Line)
                << "the host ended or stalled before it reported its helpers: " << host.stderrTail();
            if (line.line.rfind("READY", 0) != 0)
                continue;
            std::istringstream ids(line.line.substr(5));
            unsigned long pid;
            while (ids >> pid)
                helpers.push_back(pid);
            break;
        }
        ASSERT_FALSE(helpers.empty());
        if (mode == "fake")
        {
            EXPECT_EQ(helpers.size(), 4u);
        }
        for (const auto pid : helpers)
            ASSERT_TRUE(processAlive(pid)) << "pid " << pid << " should be an idle helper of the host";

        // SIGKILL for the host alone, not its process group: the helpers are not told anything
        ASSERT_EQ(::kill(static_cast<pid_t>(host.pid()), SIGKILL), 0);
        ASSERT_TRUE(host.wait(process::Session::Clock::now() + seconds(10)));

        const bool gone = waitFor(
            [&] {
                for (const auto pid : helpers)
                    if (processAlive(pid))
                        return false;
                return true;
            },
            seconds(2));
        for (const auto pid : helpers)
            EXPECT_FALSE(processAlive(pid)) << "helper " << pid << " is still there 2 s after its host was killed";
        EXPECT_TRUE(gone);
    }

    TEST(ExifHelperHost, killedHostLeavesNoFakeHelper)
    {
        expectNoHelperSurvivesAKilledHost("fake");
    }

    TEST(ExifHelperHost, killedHostLeavesNoRealHelper)
    {
        ASSERT_FALSE(realHelperPath().empty());
        expectNoHelperSurvivesAKilledHost("real");
    }
#endif
}
