/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "user_open_child.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "gdal_open_policy.h"
#include "gtest/gtest-spi.h"
#include "json.h"
#include "mio.h"
#include "process_runner.h"
#include "randomutil.h"
#include "testarea.h"
#include "utils.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace testutils
{
    namespace
    {
        // What the parent tells the child: the file it writes what the body recorded to, and which
        // call of runWithUserFileOpens in the test runs the body
        constexpr const char *kResultEnv = "DDB_TEST_USER_OPEN_RESULT";
        constexpr const char *kCallEnv = "DDB_TEST_USER_OPEN_CALL";

        // The role of this process: a child started by runWithUserFileOpens, or not (result empty)
        struct ChildRole
        {
            fs::path result;
            int call = -1;
        };

        // Read once when the binary is loaded, and removed from the environment: a program the
        // child starts (another test binary among them) is no such child
        ChildRole takeChildRole()
        {
            ChildRole role;
            const char *result = std::getenv(kResultEnv);
            const char *call = std::getenv(kCallEnv);
            if (result != nullptr && *result != '\0' && call != nullptr)
            {
                role.result = ddb::io::fromUtf8(result);
                role.call = std::atoi(call);
            }
            unsetEnv(kResultEnv);
            unsetEnv(kCallEnv);
            return role;
        }

        const ChildRole g_role = takeChildRole();

        // Counts the calls of runWithUserFileOpens in the running test
        class CallCounter : public ::testing::EmptyTestEventListener
        {
          public:
            void OnTestStart(const ::testing::TestInfo &) override { calls = 0; }
            int calls = 0;
        };

        // The index of this call in the running test (0 for the first)
        int nextCall()
        {
            // Installed by the first call, during a test that already started: it counts from 0 too
            static CallCounter *const counter = [] {
                auto *created = new CallCounter();
                ::testing::UnitTest::GetInstance()->listeners().Append(created);
                return created;
            }();
            return counter->calls++;
        }

        // The test binary that runs: the folder of the program and the name it was started with
        fs::path testBinary()
        {
            const auto &argvs = ::testing::internal::GetArgvs();
            fs::path name = argvs.empty() ? fs::path() : ddb::io::fromUtf8(argvs.front()).filename();
#ifdef _WIN32
            if (name.extension().empty())
                name += ".exe";
#endif
            return ddb::io::getExeFolderPath() / name;
        }

        // ---- the child

        // What the body recorded and is not a success
        json collect(const ::testing::TestPartResultArray &recorded)
        {
            json parts = json::array();
            for (int i = 0; i < recorded.size(); i++)
            {
                const auto &part = recorded.GetTestPartResult(i);
                if (part.type() == ::testing::TestPartResult::kSuccess)
                    continue;
                parts.push_back({{"type", static_cast<int>(part.type())},
                                 {"file", part.file_name() ? part.file_name() : "unknown"},
                                 {"line", part.line_number()},
                                 {"message", part.message()}});
            }
            return parts;
        }

        // Runs the body as the helper and gives what it recorded
        json runBody(const std::function<void()> &body)
        {
            ddb::gdalopen::enableUserFileOpens();
            std::string uncaught;
            // Everything the body reports is taken here: from this thread whatever reporter the
            // test had installed on it (a test that expects a failure has one, and a reporter of the
            // thread comes before the global one), and from any other thread
            ::testing::TestPartResultArray recorded;
            {
                ::testing::ScopedFakeTestPartResultReporter otherThreads(
                    ::testing::ScopedFakeTestPartResultReporter::INTERCEPT_ALL_THREADS, &recorded);
                ::testing::ScopedFakeTestPartResultReporter thisThread(
                    ::testing::ScopedFakeTestPartResultReporter::INTERCEPT_ONLY_CURRENT_THREAD, &recorded);
                try
                {
                    body();
                }
                catch (const std::exception &e)
                {
                    uncaught = std::string("uncaught exception: ") + e.what();
                }
                catch (...)
                {
                    uncaught = "uncaught exception of an unknown type";
                }
            }
            json parts = collect(recorded);
            if (!uncaught.empty())
                parts.push_back({{"type", static_cast<int>(::testing::TestPartResult::kFatalFailure)},
                                 {"file", "unknown"},
                                 {"line", -1},
                                 {"message", uncaught}});
            return parts;
        }

        // Ends the child once the result is written. The rest of the test (what follows the call,
        // the TearDown of the fixture) is the parent's, and the objects of the test still live, so
        // nothing is destroyed: no static destructor, no atexit handler, no DLL notification. The
        // test areas of the child go with it unless the body failed (they are what to look at).
        [[noreturn]] void endChild(bool failed)
        {
            std::error_code ec;
            if (failed)
                std::cout << "Test areas of the child that opens user files kept in "
                          << TestArea::processRoot().string() << std::endl;
            else
                fs::remove_all(TestArea::processRoot(), ec);
            std::cout.flush();
            std::cerr.flush();
            std::fflush(nullptr);
#ifdef _WIN32
            ::TerminateProcess(::GetCurrentProcess(), 0);
#endif
            ::_exit(0);
        }

        void runInChild(const std::function<void()> &body, int call)
        {
            // An earlier call of the test ran in a child of its own: the parent reported it
            if (call < g_role.call)
                return;

            const json parts = runBody(body);
            {
                std::ofstream out(g_role.result, std::ios::binary);
                out << parts.dump();
            }
            endChild(!parts.empty());
        }

        // ---- the parent

        // What the child wrote, empty when it wrote nothing
        std::string readResult(const fs::path &file)
        {
            std::ifstream in(file, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        void report(const json &parts)
        {
            std::string skipped;
            for (const auto &part : parts)
            {
                const auto type = static_cast<::testing::TestPartResult::Type>(part.value("type", 0));
                const std::string file = part.value("file", std::string("unknown"));
                const int line = part.value("line", -1);
                const std::string message = part.value("message", std::string());
                if (type == ::testing::TestPartResult::kSkip)
                    skipped = message;
                else
                    ADD_FAILURE_AT(file.c_str(), line) << "In the child that opens user files:\n" << message;
            }
            if (!skipped.empty() && !::testing::Test::HasFailure())
                GTEST_SKIP() << skipped;
        }

        void runInParent(int call)
        {
            const ::testing::TestInfo *test = ::testing::UnitTest::GetInstance()->current_test_info();
            if (test == nullptr)
            {
                ADD_FAILURE() << "runWithUserFileOpens runs in a test";
                return;
            }

            std::error_code ec;
            fs::create_directories(TestArea::processRoot(), ec);
            const fs::path result = TestArea::processRoot() / ("user-open-" + ddb::randomString(12) + ".json");

            ddb::process::Options options;
            options.envOverrides = {{kResultEnv, ddb::io::toUtf8(result)},
                                    {kCallEnv, std::to_string(call)},
                                    // The report of the parent is the parent's to write
                                    {"GTEST_OUTPUT", ""}};
            // The test has the time limit of the run (ctest), as any test does
            options.idleTimeout = std::chrono::milliseconds(0);
            options.onOutputLine = [](const std::string &line) { std::cout << line << std::endl; };
            const std::vector<std::string> args = {
                std::string("--gtest_filter=") + test->test_suite_name() + "." + test->name(),
                "--gtest_also_run_disabled_tests", "--gtest_repeat=1", "--gtest_shuffle=0", "--gtest_brief=1"};

            std::cout.flush();
            std::fflush(nullptr);
            const ddb::process::Result run = ddb::process::run(testBinary(), args, options);

            const std::string received = readResult(result);
            fs::remove(result, ec);
            if (run.termSignal != 0)
            {
                ADD_FAILURE() << "the child that opens user files died of signal " << run.termSignal;
                return;
            }
            if (!run.success() || received.empty())
            {
                ADD_FAILURE() << "the child that opens user files ended without a result (" << run.describe()
                              << "): its test failed before the call, or does not run alone";
                return;
            }

            const json parts = json::parse(received, nullptr, false);
            if (!parts.is_array())
            {
                ADD_FAILURE() << "the child that opens user files sent an unreadable result";
                return;
            }
            report(parts);
        }
    } // namespace

    void runWithUserFileOpens(const std::function<void()> &body)
    {
        const int call = nextCall();
        if (!g_role.result.empty())
            runInChild(body, call);
        else
            runInParent(call);
    }
} // namespace testutils
