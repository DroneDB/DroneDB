/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "user_open_child.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>

#include "gdal_open_policy.h"
#include "gtest/gtest-spi.h"
#include "json.h"
#include "testarea.h"

#ifndef _WIN32
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace testutils
{
#ifdef _WIN32

    void runWithUserFileOpens(const std::function<void()> &body)
    {
        ddb::gdalopen::enableUserFileOpens();
        body();
    }

#else

    namespace
    {
        // What the child sends: the parts that its body recorded and are not a success
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

        void writeAll(int fd, const std::string &text)
        {
            size_t done = 0;
            while (done < text.size())
            {
                const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
                if (n <= 0)
                    return;
                done += static_cast<size_t>(n);
            }
        }

        [[noreturn]] void runChild(int fd, const std::function<void()> &body)
        {
            ddb::gdalopen::enableUserFileOpens();
            std::string uncaught;
            // Everything the body reports is taken here and sent: from this thread whatever
            // reporter the parent had installed on it (a test that expects a failure has one, and
            // a reporter of the thread comes before the global one), and from any other thread
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
            writeAll(fd, parts.dump());
            std::cout.flush();
            std::fflush(nullptr);
            // No static destructors and no atexit handlers: the child is a copy of a process with
            // threads it does not have, and it has nothing left to release
            ::_exit(0);
        }

        std::string readAll(int fd)
        {
            std::string text;
            char buffer[4096];
            for (;;)
            {
                const ssize_t n = ::read(fd, buffer, sizeof(buffer));
                if (n < 0 && errno == EINTR)
                    continue;
                if (n <= 0)
                    return text;
                text.append(buffer, static_cast<size_t>(n));
            }
        }
    } // namespace

    void runWithUserFileOpens(const std::function<void()> &body)
    {
        // The folder of the areas of this process is chosen by its first use: the child must
        // find the one of the parent, which removes it when it ends
        (void)TestArea::processRoot();

        int fds[2];
        ASSERT_EQ(::pipe(fds), 0) << std::strerror(errno);
        std::cout.flush();
        std::fflush(nullptr);

        const pid_t pid = ::fork();
        if (pid < 0)
        {
            ::close(fds[0]);
            ::close(fds[1]);
            FAIL() << "fork failed: " << std::strerror(errno);
        }
        if (pid == 0)
        {
            ::close(fds[0]);
            runChild(fds[1], body);
        }

        ::close(fds[1]);
        const std::string received = readAll(fds[0]);
        ::close(fds[0]);
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR)
        {
        }

        if (WIFSIGNALED(status))
        {
            ADD_FAILURE() << "the child that opens user files died of signal " << WTERMSIG(status);
            return;
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || received.empty())
        {
            ADD_FAILURE() << "the child that opens user files ended without a result (status " << status << ")";
            return;
        }

        const json parts = json::parse(received, nullptr, false);
        if (!parts.is_array())
        {
            ADD_FAILURE() << "the child that opens user files sent an unreadable result";
            return;
        }
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

#endif
} // namespace testutils
