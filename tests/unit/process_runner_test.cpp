/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Shared lifecycle tests use a portable child; shell/descriptor tests remain
// platform-specific, and tests that inspect /proc are Linux-only.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "fs.h"
#include "gtest/gtest.h"
#include "mio.h"
#include "process_runner.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"
#include "helpers/process_test_utils.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using namespace ddb;
using std::chrono::milliseconds;

#ifdef _WIN32
#define SKIP_ON_WINDOWS() GTEST_SKIP() << "process runner tests drive /bin/sh (POSIX only)"
#else
#define SKIP_ON_WINDOWS() \
    do {                  \
    } while (0)
#endif

#ifdef __linux__
#define SKIP_WITHOUT_PROC() \
    do {                    \
    } while (0)
#else
#define SKIP_WITHOUT_PROC() GTEST_SKIP() << "needs Linux /proc"
#endif

const fs::path kSh = "/bin/sh";

process::Result sh(const std::string& script, process::Options opts = process::Options(),
                   const std::vector<std::string>& extraArgs = {}) {
    std::vector<std::string> args = {"-c", script, "sh"};
    args.insert(args.end(), extraArgs.begin(), extraArgs.end());
    return process::run(kSh, args, opts);
}

std::string readText(const fs::path& p) {
    std::ifstream in(p.string(), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string l;
    while (std::getline(in, l))
        if (!l.empty()) out.push_back(l);
    return out;
}

// True when pid no longer exists or is a zombie (dead but not yet reaped by its
// new parent). Waits up to `within`.
bool processGone(unsigned long pid, milliseconds within) {
    const auto deadline = std::chrono::steady_clock::now() + within;
    for (;;) {
        bool gone = false;
#ifdef _WIN32
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
        if (process) {
            gone = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
            CloseHandle(process);
        } else {
            gone = GetLastError() == ERROR_INVALID_PARAMETER;
        }
#else
        if (::kill(pid, 0) != 0 && errno == ESRCH) {
            gone = true;
        } else {
#ifdef __linux__
            const std::string stat = readText("/proc/" + std::to_string(pid) + "/stat");
            const auto rp = stat.rfind(')');
            gone = stat.empty() || (rp != std::string::npos && rp + 2 < stat.size() &&
                                    (stat[rp + 2] == 'Z' || stat[rp + 2] == 'X'));
#endif
        }
#endif
        if (gone) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(milliseconds(20));
    }
}

bool waitForFile(const fs::path& p, milliseconds within) {
    const auto deadline = std::chrono::steady_clock::now() + within;
    while (std::chrono::steady_clock::now() < deadline) {
        if (fs::exists(p) && !readText(p).empty()) return true;
        std::this_thread::sleep_for(milliseconds(20));
    }
    return false;
}

TEST(processRunner, exitCodePropagated) {
    const auto r = process::run(testChildPath(), {"exit", "7"});
    EXPECT_TRUE(r.started);
    EXPECT_TRUE(r.exited);
    EXPECT_EQ(r.exitCode, 7);
    EXPECT_EQ(r.termSignal, 0);
    EXPECT_FALSE(r.timedOut);
    EXPECT_FALSE(r.descendantsTerminated);
    EXPECT_FALSE(r.success());
    EXPECT_EQ(r.outputTail, "hello\n");
    EXPECT_EQ(r.totalBytes, 6u);
    EXPECT_NE(r.describe().find("exited with code 7"), std::string::npos) << r.describe();

    const auto ok = process::run(testChildPath(), {"exit", "0"});
    EXPECT_TRUE(ok.success());
}

// The output pipe reaches EOF a moment before the exiting child can be reaped: run() must then
// wake on the exit itself, not on the next tick of a coarse poll that would add tens of
// milliseconds to every short tool call. The median keeps a loaded machine from failing it.
// Sanitizer builds skip it: there the start of the instrumented child alone takes ~30 ms. So
// does Windows, where run() waits on the process handle and creating a process alone takes
// about 50 ms.
TEST(processRunner, shortChildReturnsPromptly) {
    SKIP_ON_WINDOWS();
    if (std::getenv("DDB_SKIP_TIMING_TESTS") != nullptr)
        GTEST_SKIP() << "timing test disabled";
    constexpr int kRuns = 50;
    std::vector<double> ms;
    ms.reserve(kRuns);
    for (int i = 0; i < kRuns; ++i) {
        const auto start = std::chrono::steady_clock::now();
        const auto r = process::run(testChildPath(), {"exit", "0"});
        ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        ASSERT_TRUE(r.success()) << r.describe();
    }
    std::sort(ms.begin(), ms.end());
    const double median = ms[kRuns / 2];
    std::cout << "process::run of a short child: median " << median << " ms, min " << ms.front()
              << " ms, max " << ms.back() << " ms over " << kRuns << " runs" << std::endl;
    EXPECT_LT(median, 25.0);
}

TEST(processRunner, stdoutAndStderrMerged) {
    SKIP_ON_WINDOWS();
    std::vector<std::string> lines;
    process::Options opts;
    opts.onOutputLine = [&lines](const std::string& l) { lines.push_back(l); };
    const auto r = sh("echo out; echo err >&2; printf 'cr\\rlast'", opts);
    ASSERT_TRUE(r.success()) << r.describe();
    EXPECT_EQ(lines, (std::vector<std::string>{"out", "err", "cr", "last"}));
}

TEST(processRunner, signalNumberReported) {
    SKIP_ON_WINDOWS();
    const auto r = sh("kill -TERM $$; sleep 5");
    EXPECT_TRUE(r.started);
    EXPECT_FALSE(r.exited);
    EXPECT_FALSE(r.timedOut);
#ifndef _WIN32
    EXPECT_EQ(r.termSignal, SIGTERM);
#endif
    EXPECT_NE(r.describe().find("signal 15"), std::string::npos) << r.describe();
}

TEST(processRunner, stdinIsNullDevice) {
    SKIP_ON_WINDOWS();
    process::Options opts;
    opts.idleTimeout = std::chrono::seconds(20);
    const auto start = std::chrono::steady_clock::now();
    // cat gets EOF immediately: it must not wait on the parent's stdin.
    const auto r = sh("cat; echo done", opts);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    ASSERT_TRUE(r.success()) << r.describe();
    EXPECT_EQ(r.outputTail, "done\n");
    EXPECT_LT(elapsed, std::chrono::seconds(10));

#ifdef __linux__
    const auto link = sh("readlink /proc/self/fd/0");
    ASSERT_TRUE(link.success());
    EXPECT_EQ(link.outputTail, "/dev/null\n");
#endif
}

TEST(processRunner, onlyStandardFdsInherited) {
    SKIP_ON_WINDOWS();
    SKIP_WITHOUT_PROC();
#ifndef _WIN32
    // A descriptor the parent holds WITHOUT close-on-exec, at a high number.
    const int raw = ::open("/dev/null", O_RDONLY);
    ASSERT_GE(raw, 0);
    const int leaked = ::fcntl(raw, F_DUPFD, 57);
    ::close(raw);
    ASSERT_GE(leaked, 57);

    // `ls` lists the shell's descriptor table (its own directory fd is not in it).
    const std::string listFds = "ls /proc/$$/fd";

    const auto r = sh(listFds);
    ASSERT_TRUE(r.success()) << r.describe() << " " << r.outputTail;
    std::set<int> fds;
    for (const auto& l : splitLines(r.outputTail)) fds.insert(std::stoi(l));
    EXPECT_EQ(fds, (std::set<int>{0, 1, 2})) << r.outputTail;

    // With the extra channel, exactly one more descriptor (the fixed number).
    process::Options opts;
    opts.extraChannel = true;
    const auto r2 = sh(listFds + "; echo fd=$1", opts, {process::kExtraChannelPlaceholder});
    ASSERT_TRUE(r2.success()) << r2.describe();
    std::set<int> fds2;
    std::string fdArg;
    for (const auto& l : splitLines(r2.outputTail)) {
        if (l.rfind("fd=", 0) == 0)
            fdArg = l.substr(3);
        else
            fds2.insert(std::stoi(l));
    }
    EXPECT_EQ(fds2, (std::set<int>{0, 1, 2, process::kExtraChannelFd})) << r2.outputTail;
    EXPECT_EQ(fdArg, std::to_string(process::kExtraChannelFd));

    ::close(leaked);
#endif
}

TEST(processRunner, idleTimeoutKillsProcessGroup) {
    TestArea ta(TEST_NAME);
    const fs::path pidFile = ta.getPath("grandchild.pid");
    io::assureIsRemoved(pidFile);

    process::Options opts;
    opts.idleTimeout = milliseconds(800);
    const auto start = std::chrono::steady_clock::now();
    const auto r = process::run(testChildPath(), {"descendant", pidFile.string(), "silent", "30000", "0"}, opts);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(r.started);
    EXPECT_TRUE(r.timedOut);
    EXPECT_FALSE(r.success());
#ifndef _WIN32
    EXPECT_EQ(r.termSignal, SIGKILL);
#endif
    EXPECT_FALSE(r.descendantsTerminated);
    EXPECT_NE(r.describe().find("without output"), std::string::npos) << r.describe();
    EXPECT_LT(elapsed, std::chrono::seconds(10));

    ASSERT_TRUE(waitForFile(pidFile, milliseconds(2000)));
    const auto grandchild = std::stoul(readText(pidFile));
    ASSERT_GT(grandchild, 0);
    EXPECT_TRUE(processGone(grandchild, milliseconds(3000))) << "grandchild " << grandchild << " survived";
}

// A child that closes every monitored pipe but keeps running is still
// subject to the idle timeout (measured from its last output).
TEST(processRunner, idleTimeoutAppliesAfterPipesClosed) {
    process::Options opts;
    opts.idleTimeout = milliseconds(800);
    const auto start = std::chrono::steady_clock::now();
    const auto r = process::run(testChildPath(), {"close-output", "30000", "0"}, opts);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(r.started);
    EXPECT_TRUE(r.timedOut) << r.describe();
    EXPECT_FALSE(r.success());
#ifndef _WIN32
    EXPECT_EQ(r.termSignal, SIGKILL);
#endif
    EXPECT_NE(r.outputTail.find("before-close"), std::string::npos);
    EXPECT_GE(elapsed, milliseconds(700));
    EXPECT_LT(elapsed, std::chrono::seconds(10));

    // Control: closing the pipes and exiting before the deadline is a normal exit.
    const auto quick = process::run(testChildPath(), {"close-output", "200", "4"}, opts);
    EXPECT_FALSE(quick.timedOut) << quick.describe();
    EXPECT_TRUE(quick.exited);
    EXPECT_EQ(quick.exitCode, 4);

    // Exercise closure of the progress channel as well as stdout/stderr.
    opts.extraChannel = true;
    const auto extra = process::run(testChildPath(),
        {"close-output", "30000", "0", process::kExtraChannelPlaceholder}, opts);
    EXPECT_TRUE(extra.timedOut) << extra.describe();
    EXPECT_LT(extra.duration, std::chrono::seconds(10));
}

// A child that never stops writing never trips the idle timeout; maxRuntime
// still bounds it (dwg2dxf can loop while printing warnings).
TEST(processRunner, maxRuntimeKillsChattyChild) {
    process::Options opts;
    opts.idleTimeout = milliseconds(800);
    opts.maxRuntime = milliseconds(1500);
    const auto start = std::chrono::steady_clock::now();
    const auto r = process::run(testChildPath(), {"descendant-worker", "streaming"}, opts);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(r.started);
    EXPECT_TRUE(r.timedOut) << r.describe();
    EXPECT_FALSE(r.success());
    EXPECT_GT(r.totalBytes, 0u);
    EXPECT_NE(r.describe().find("time limit"), std::string::npos) << r.describe();
    EXPECT_GE(elapsed, milliseconds(1400));
    EXPECT_LT(elapsed, std::chrono::seconds(10));
}

// maxRuntime also applies after the child closed its pipes, with the idle
// timeout disabled.
TEST(processRunner, maxRuntimeAppliesAfterPipesClosed) {
    process::Options opts;
    opts.idleTimeout = milliseconds(0);
    opts.maxRuntime = milliseconds(800);
    const auto r = process::run(testChildPath(), {"close-output", "30000", "0"}, opts);

    EXPECT_TRUE(r.timedOut) << r.describe();
    EXPECT_NE(r.describe().find("time limit"), std::string::npos) << r.describe();
    EXPECT_LT(r.duration, std::chrono::seconds(10));

    // Control: a quick child finishes normally under the same limit.
    const auto quick = process::run(testChildPath(), {"exit", "0"}, opts);
    EXPECT_TRUE(quick.success()) << quick.describe();
}

// Activity on the extra channel alone keeps the child alive.
TEST(processRunner, extraChannelActivityResetsIdleTimer) {
    std::string extra;
    process::Options opts;
    opts.extraChannel = true;
    opts.idleTimeout = milliseconds(700);
    opts.onExtraData = [&extra](const char* d, size_t n) { extra.append(d, n); };

    const auto start = std::chrono::steady_clock::now();
    const auto r = process::run(testChildPath(), {"extra-ticks", process::kExtraChannelPlaceholder}, opts);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(r.timedOut) << r.describe();
    EXPECT_TRUE(r.success()) << r.describe();
    EXPECT_EQ(r.totalBytes, 0u); // nothing on stdout/stderr
    std::string expectedTicks;
    for (int i = 0; i < 8; ++i) expectedTicks += "tick\n";
    EXPECT_EQ(extra, expectedTicks);
    // The child outlived the idle timeout several times over.
    EXPECT_GT(elapsed, milliseconds(1000));

    // Control: the same idle timeout kills a child that is silent everywhere.
    const auto silent = process::run(testChildPath(),
        {"close-output", "5000", "0", process::kExtraChannelPlaceholder}, opts);
    EXPECT_TRUE(silent.timedOut);
}

TEST(processRunner, ringBufferKeepsTail) {
    SKIP_ON_WINDOWS();
    size_t lineCount = 0;
    process::Options opts;
    opts.outputCapacity = 100;
    opts.onOutputLine = [&lineCount](const std::string&) { ++lineCount; };
    const auto r = sh("i=1; while [ $i -le 3000 ]; do echo $i; i=$((i+1)); done", opts);
    ASSERT_TRUE(r.success()) << r.describe();

    std::string expected;
    for (int i = 1; i <= 3000; ++i) expected += std::to_string(i) + "\n";
    EXPECT_EQ(r.totalBytes, expected.size());
    EXPECT_EQ(r.outputTail, expected.substr(expected.size() - 100));
    EXPECT_EQ(lineCount, 3000u);
    EXPECT_EQ(r.tail(10), "..." + expected.substr(expected.size() - 10));
}

TEST(processRunner, envOverrideReplacesInheritedValue) {
    SKIP_ON_WINDOWS();
#ifndef _WIN32
    setEnv("DDB_PR_TEST_A", "inherited");
    setEnv("DDB_PR_TEST_B", "kept");
    unsetEnv("DDB_PR_TEST_C");

    process::Options opts;
    opts.envOverrides = {{"DDB_PR_TEST_A", "override"}, {"DDB_PR_TEST_C", "added"}};
    const auto r = sh("env | grep '^DDB_PR_TEST_' | sort", opts);

    // The parent's own environment is untouched.
    EXPECT_STREQ(std::getenv("DDB_PR_TEST_A"), "inherited");
    EXPECT_EQ(std::getenv("DDB_PR_TEST_C"), nullptr);

    unsetEnv("DDB_PR_TEST_A");
    unsetEnv("DDB_PR_TEST_B");

    ASSERT_TRUE(r.success()) << r.describe();
    EXPECT_EQ(splitLines(r.outputTail),
              (std::vector<std::string>{"DDB_PR_TEST_A=override", "DDB_PR_TEST_B=kept", "DDB_PR_TEST_C=added"}));

    // Without overrides the environment is inherited unchanged.
    setEnv("DDB_PR_TEST_A", "plain");
    const auto r2 = sh("printf '%s' \"$DDB_PR_TEST_A\"");
    unsetEnv("DDB_PR_TEST_A");
    EXPECT_EQ(r2.outputTail, "plain");
#endif
}

TEST(processRunner, execFailureReported) {
    SKIP_ON_WINDOWS();
    const auto missing = process::run("/nonexistent/ddb_missing_tool", {"--help"});
    EXPECT_FALSE(missing.started);
    EXPECT_FALSE(missing.success());
    EXPECT_NE(missing.describe().find("failed to start"), std::string::npos) << missing.describe();
    EXPECT_NE(missing.describe().find("No such file"), std::string::npos) << missing.describe();

    TestArea ta(TEST_NAME);
    const fs::path notExec = ta.getPath("not_executable.sh");
    {
        std::ofstream o(notExec.string());
        o << "#!/bin/sh\necho hi\n";
    }
    fs::permissions(notExec, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    const auto denied = process::run(notExec, {});
    EXPECT_FALSE(denied.started);
    EXPECT_NE(denied.describe().find("Permission denied"), std::string::npos) << denied.describe();
}

// The parent's pipe ends and /dev/null are owned by UniqueFd, so every exit path of
// run() (success, extra channel, exec failure, timeout kill) closes all of them.
TEST(processRunner, parentDescriptorsClosedOnEveryPath) {
    SKIP_ON_WINDOWS();
    SKIP_WITHOUT_PROC();
    const auto openDescriptors = [] {
        return std::distance(fs::directory_iterator("/proc/self/fd"), fs::directory_iterator());
    };
    const auto before = openDescriptors();

    EXPECT_TRUE(sh("echo out").success());

    process::Options extra;
    extra.extraChannel = true;
    EXPECT_TRUE(sh("echo progress >&$1", extra, {process::kExtraChannelPlaceholder}).success());

    EXPECT_FALSE(process::run("/nonexistent/ddb_missing_tool", {}).started);

    process::Options idle;
    idle.idleTimeout = milliseconds(200);
    EXPECT_TRUE(sh("exec sleep 30", idle).timedOut);

    EXPECT_EQ(openDescriptors(), before);
}

TEST(processRunner, scriptWithShebangRuns) {
    SKIP_ON_WINDOWS();
    TestArea ta(TEST_NAME);
    const fs::path script = ta.getPath("script.sh");
    {
        std::ofstream o(script.string());
        o << "#!/bin/sh\necho \"args:$#:$1:$2\"\nexit 3\n";
    }
    fs::permissions(script, fs::perms::owner_all, fs::perm_options::replace);
    const auto r = process::run(script, {"a b", "c"});
    ASSERT_TRUE(r.started) << r.describe();
    EXPECT_EQ(r.exitCode, 3);
    EXPECT_EQ(r.outputTail, "args:2:a b:c\n");
}

// The direct child exits but a background descendant keeps the output pipe open:
// run() returns after the drain grace period and kills the straggler.
TEST(processRunner, strayDescendantDoesNotBlock) {
    TestArea ta(TEST_NAME);
    const fs::path pidFile = ta.getPath("stray.pid");
    io::assureIsRemoved(pidFile);

    const auto start = std::chrono::steady_clock::now();
    const auto r = process::run(testChildPath(), {"descendant", pidFile.string(), "silent", "0", "4"});
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(r.exited);
    EXPECT_EQ(r.exitCode, 4);
    EXPECT_FALSE(r.timedOut);
    EXPECT_TRUE(r.descendantsTerminated);
    EXPECT_NE(r.describe().find("stray descendants terminated"), std::string::npos) << r.describe();
    EXPECT_LT(elapsed, process::kExitDrainGrace + std::chrono::seconds(2));

    ASSERT_TRUE(waitForFile(pidFile, milliseconds(2000)));
    const auto stray = std::stoul(readText(pidFile));
    EXPECT_TRUE(processGone(stray, milliseconds(3000))) << "descendant " << stray << " survived";
}

class ProcessRunnerDescendant : public ::testing::TestWithParam<std::string> {};

// Activity must neither hide direct exit nor postpone the drain deadline. With
// silent descendants the short idle timeout must not replace child success.
TEST_P(ProcessRunnerDescendant, successPreservedAndDrainDeadlineEnforced) {
    TestArea ta(TEST_NAME);
    const auto pidFile = ta.getPath("stray-" + GetParam() + ".pid");
    io::assureIsRemoved(pidFile);
    process::Options opts;
    opts.idleTimeout = milliseconds(800);
    opts.extraChannel = GetParam() == "extra";
    size_t extraBytes = 0;
    opts.onExtraData = [&extraBytes](const char*, size_t n) { extraBytes += n; };
    std::vector<std::string> args = {"descendant", pidFile.string(), GetParam(), "0", "0"};
    if (opts.extraChannel) args.push_back(process::kExtraChannelPlaceholder);
    const auto start = std::chrono::steady_clock::now();
    const auto r = process::run(testChildPath(), args, opts);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(r.success()) << r.describe();
    EXPECT_TRUE(r.descendantsTerminated) << r.describe();
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 0);
    EXPECT_GE(elapsed, process::kExitDrainGrace);
    EXPECT_LT(elapsed, process::kExitDrainGrace + std::chrono::seconds(2));
    if (GetParam() == "streaming") EXPECT_GT(r.totalBytes, 0u);
    if (GetParam() == "extra") EXPECT_GT(extraBytes, 0u);
    ASSERT_TRUE(waitForFile(pidFile, milliseconds(2000)));
    const auto stray = std::stoul(readText(pidFile));
    EXPECT_TRUE(processGone(stray, milliseconds(3000))) << "descendant " << stray << " survived";
}

INSTANTIATE_TEST_SUITE_P(portable, ProcessRunnerDescendant,
                        ::testing::Values("silent", "streaming", "extra"));

// Linux PR_SET_PDEATHSIG: when the process that spawned the child dies, the child
// is killed too. An intermediate process (forked from the test) runs the helper
// and is then SIGKILLed; the helper's child must die with it.
TEST(processRunner, childDiesWithSpawningProcess) {
    SKIP_ON_WINDOWS();
    SKIP_WITHOUT_PROC();
#ifdef __linux__
    TestArea ta(TEST_NAME);
    const fs::path pidFile = ta.getPath("child.pid");
    io::assureIsRemoved(pidFile);

    const pid_t intermediate = ::fork();
    ASSERT_NE(intermediate, -1);
    if (intermediate == 0) {
        process::Options opts;
        opts.idleTimeout = std::chrono::seconds(60);
        process::run(kSh, {"-c", "echo $$ > \"$1\"; exec sleep 60", "sh", pidFile.string()}, opts);
        ::_exit(0);
    }

    const bool started = waitForFile(pidFile, milliseconds(5000));
    ::kill(intermediate, SIGKILL);
    int status = 0;
    ::waitpid(intermediate, &status, 0);
    ASSERT_TRUE(started) << "helper child did not start";

    const pid_t child = static_cast<pid_t>(std::stol(readText(pidFile)));
    ASSERT_GT(child, 0);
    EXPECT_TRUE(processGone(child, milliseconds(3000))) << "child " << child << " survived its parent";
    if (!processGone(child, milliseconds(0))) ::kill(child, SIGKILL);
#endif
}

TEST(processRunner, formatCommandLineQuotes) {
    const std::string s = process::formatCommandLine("/usr/bin/tool", {"plain", "with space"});
    EXPECT_EQ(s.find("/usr/bin/tool plain "), 0u) << s;
    EXPECT_NE(s.find("with space"), std::string::npos);
    EXPECT_NE(s.back(), 'e'); // the spaced argument is quoted
}

}  // namespace
