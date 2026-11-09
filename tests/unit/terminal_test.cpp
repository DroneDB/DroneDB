/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The prompts of the CLI (cmd::terminal) without a terminal: stdin from the null device, as in a
// script, a CI job or `ddb push < /dev/null`. getPass called tcsetattr on it, printed
// "tcsetattr: Inappropriate ioctl for device" and ended the process with exit(1); on Windows it
// read an empty password. Both prompts now fail with one message, which the CLI prints like any
// other error (exit code 1), before "Username:" is printed.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <io.h> // _sopen_s, _SH_DENYNO
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "cmd/include/cli.h"
#include "cmd/include/credentials.h"
#include "cmd/include/login.h"
#include "cmd/include/push.h"
#include "cmd/include/terminal.h"
#include "exceptions.h"
#include "fake_http_client.h"
#include "gtest/gtest.h"
#include "tagmanager.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace
{
    constexpr const char *NoTerminal =
        "no terminal to read the password: run `ddb login -u <user> -p <password>` first "
        "(ddb share also reads DDB_USERNAME and DDB_PASSWORD)";

    // stdin from the null device for the scope of the object, then back to what it was
    class NullStdin
    {
        int saved = -1;
#ifdef _WIN32
        HANDLE savedHandle = nullptr;
#endif

    public:
        NullStdin()
        {
#ifdef _WIN32
            savedHandle = GetStdHandle(STD_INPUT_HANDLE);
            saved = _dup(0);
            int null = -1;
            _sopen_s(&null, "NUL", _O_RDONLY, _SH_DENYNO, 0);
            _dup2(null, 0);
            _close(null);
            // terminal.cpp reads the handle of the process, not the descriptor of the CRT
            SetStdHandle(STD_INPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(0)));
#else
            saved = dup(0);
            const int null = open("/dev/null", O_RDONLY);
            dup2(null, 0);
            close(null);
#endif
            clearerr(stdin);
        }
        ~NullStdin()
        {
#ifdef _WIN32
            if (saved >= 0)
            {
                _dup2(saved, 0);
                _close(saved);
            }
            SetStdHandle(STD_INPUT_HANDLE, savedHandle);
#else
            if (saved >= 0)
            {
                dup2(saved, 0);
                close(saved);
            }
#endif
            clearerr(stdin);
        }
        NullStdin(const NullStdin &) = delete;
        NullStdin &operator=(const NullStdin &) = delete;
    };

    struct Outcome
    {
        int code = -1;
        std::string out;
        std::string err;
    };

    // Runs a command line through the handler of the CLI, capturing stdout
    Outcome execute(cmd::Command &command, std::vector<std::string> args)
    {
        std::vector<char *> argv{const_cast<char *>("ddb")};
        for (auto &a : args)
            argv.push_back(a.data());

        std::ostringstream out, err;
        std::streambuf *previous = std::cout.rdbuf(out.rdbuf());
        Outcome outcome;
        try
        {
            outcome.code = cmd::execute(command, static_cast<int>(argv.size()), argv.data(), err);
        }
        catch (...)
        {
            std::cout.rdbuf(previous);
            throw;
        }
        std::cout.rdbuf(previous);
        outcome.out = out.str();
        outcome.err = err.str();
        return outcome;
    }

    TEST(Terminal, bothPromptsRefuseAStdinThatIsNotATerminal)
    {
        NullStdin input;
        for (const auto &read : {cmd::terminal::getPass, cmd::terminal::getPrompt})
        {
            try
            {
                read("Prompt: ");
                FAIL() << "read a prompt from the null device";
            }
            catch (const ddb::AuthException &e)
            {
                EXPECT_STREQ(e.what(), NoTerminal);
            }
        }
    }

    // `ddb login` without -u and -p, as a script runs it: the error and exit code 1, not
    // "Username:" and a process that perror ended
    TEST(Terminal, loginWithoutATerminalFailsWithTheMessageAndExitCode1)
    {
        NullStdin input;
        cmd::Login login;
        const auto outcome = execute(login, {"--server", "http://127.0.0.1:1"});
        EXPECT_EQ(outcome.code, EXIT_FAILURE);
        EXPECT_EQ(outcome.err, std::string(NoTerminal) + "\n");
        EXPECT_EQ(outcome.out.find("Username"), std::string::npos) << outcome.out;
    }

    // `ddb push` with nothing saved, against a Registry that answers: the real provider of the
    // CLI (the terminal) is asked, and without a terminal the push fails the same way
    TEST(Terminal, pushWithNothingSavedAndNoTerminalFailsWithTheMessage)
    {
        IndexFixture dataset("terminal_push_no_tty");
        ddb::TagManager(dataset.db.get()).setTag("https://registry.test/org/ds");
        dataset.db->close();
        fake::Network net(dataset.ta.getPath("auth.json"));
        net.http.on("GET", "/sys/version", fake::jsonReply({{"version", "3.0.0"}}));
        net.acceptLogin();

        const auto previous = std::filesystem::current_path();
        std::filesystem::current_path(dataset.root);
        Outcome outcome;
        {
            NullStdin input;
            cmd::Push push(cmd::RegistryServices{nullptr, net.deps()}); // the terminal answers
            outcome = execute(push, {});
        }
        std::filesystem::current_path(previous);

        EXPECT_EQ(outcome.code, EXIT_FAILURE);
        EXPECT_EQ(outcome.err, std::string(NoTerminal) + "\n");
        EXPECT_EQ(outcome.out.find("Username"), std::string::npos) << outcome.out;
        EXPECT_TRUE(net.http.callsTo("POST", "/users/authenticate").empty());
    }
} // namespace
