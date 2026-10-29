/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// process::Session against the ddbtest_child helper: line framing and pipelining, deadlines,
// end of input, kill and exit status, a child that is gone, a noisy stderr, and many sessions
// alive at once. Waits are bounded deadlines, never sleeps used for synchronization.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "fs.h"
#include "gtest/gtest.h"
#include "mio.h"
#include "process_runner.h"
#include "helpers/process_test_utils.h"

#ifndef _WIN32
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

using namespace ddb;
using process::Session;
using std::chrono::milliseconds;
using std::chrono::seconds;

// How long a step that must succeed may take before the test calls it hung.
constexpr seconds kPatience(20);

Session::Clock::time_point within(milliseconds ms) { return Session::Clock::now() + ms; }
Session::Clock::time_point patiently() { return Session::Clock::now() + kPatience; }

void startChild(Session& session, const std::vector<std::string>& args,
                const process::SessionOptions& options = process::SessionOptions()) {
    ASSERT_TRUE(session.start(testChildPath(), args, options)) << session.startError();
}

// One request, one answer
std::string ask(Session& session, const std::string& request) {
    EXPECT_EQ(session.writeLine(request, patiently()), Session::WriteStatus::Ok);
    const auto reply = session.readLine(patiently(), 4 * 1024 * 1024);
    EXPECT_EQ(reply.status, Session::ReadStatus::Line);
    return reply.line;
}

// Closes stdin and waits for the child to end by itself
void finishNormally(Session& session) {
    session.closeStdin();
    ASSERT_TRUE(session.wait(patiently())) << "the child did not end after its stdin was closed";
    ASSERT_TRUE(session.finished());
    EXPECT_TRUE(session.exitStatus().exited);
    EXPECT_EQ(session.exitStatus().exitCode, 0) << session.describeExit();
}

#ifndef _WIN32
// SIGPIPE with its default action and unblocked in the calling thread, as in a host that does not
// care about it; the previous state comes back at the end.
class DefaultSigpipe {
  public:
    DefaultSigpipe() {
        struct sigaction dfl {};
        dfl.sa_handler = SIG_DFL;
        sigemptyset(&dfl.sa_mask);
        sigaction(SIGPIPE, &dfl, &oldAction_);
        sigset_t pipeSignal;
        sigemptyset(&pipeSignal);
        sigaddset(&pipeSignal, SIGPIPE);
        pthread_sigmask(SIG_UNBLOCK, &pipeSignal, &oldMask_);
    }
    ~DefaultSigpipe() {
        pthread_sigmask(SIG_SETMASK, &oldMask_, nullptr);
        sigaction(SIGPIPE, &oldAction_, nullptr);
    }

  private:
    struct sigaction oldAction_ {};
    sigset_t oldMask_;
};
#endif

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
    do {                       \
    } while (0)
#endif

}  // namespace

TEST(processSession, lineRoundTripAndPipelinedRequests) {
    Session session;
    startChild(session, {"echo"});
    EXPECT_TRUE(session.started());
    EXPECT_GT(session.pid(), 0u);

    EXPECT_EQ(ask(session, "hello"), "echo:hello");

    // Three requests written before any answer is read come back in order
    for (const char* request : {"a", "b", "c"})
        ASSERT_EQ(session.writeLine(request, patiently()), Session::WriteStatus::Ok);
    for (const char* expected : {"echo:a", "echo:b", "echo:c"}) {
        const auto reply = session.readLine(patiently(), 1024);
        ASSERT_EQ(reply.status, Session::ReadStatus::Line);
        EXPECT_EQ(reply.line, expected);
    }
    finishNormally(session);
}

TEST(processSession, largeLineCrossesPipeAndSocketBuffers) {
    Session session;
    startChild(session, {"echo"});
    const std::string request(1024 * 1024, 'q');
    EXPECT_EQ(ask(session, request), "echo:" + request);
    finishNormally(session);
}

TEST(processSession, readTimeoutKeepsTheChildAndTheData) {
    Session session;
    startChild(session, {"echo"});

    // Nothing was asked: a deadline in the past and a short one both time out
    EXPECT_EQ(session.readLine(Session::Clock::now() - seconds(1), 1024).status, Session::ReadStatus::Timeout);
    const auto start = Session::Clock::now();
    EXPECT_EQ(session.readLine(within(milliseconds(150)), 1024).status, Session::ReadStatus::Timeout);
    EXPECT_GE(Session::Clock::now() - start, milliseconds(100));

    // The child was not killed and the session still works
    EXPECT_FALSE(session.finished());
    EXPECT_EQ(ask(session, "after"), "echo:after");
    finishNormally(session);
}

TEST(processSession, partialLineThenEof) {
    Session session;
    startChild(session, {"partial-eof"});

    const auto first = session.readLine(patiently(), 1024);
    EXPECT_EQ(first.status, Session::ReadStatus::Eof);
    EXPECT_EQ(first.line, "partial");

    const auto again = session.readLine(patiently(), 1024);
    EXPECT_EQ(again.status, Session::ReadStatus::Eof);
    EXPECT_TRUE(again.line.empty());

    ASSERT_TRUE(session.wait(patiently()));
    EXPECT_EQ(session.exitStatus().exitCode, 0);
}

TEST(processSession, lineLongerThanTheLimitIsReported) {
    const auto lineOf = [](size_t length, size_t limit) {
        Session session;
        startChild(session, {"long-line", std::to_string(length)});
        auto reply = session.readLine(patiently(), limit);
        session.kill();
        return reply;
    };

    // A line of exactly the limit is accepted, one byte more is not
    const auto exact = lineOf(100, 100);
    EXPECT_EQ(exact.status, Session::ReadStatus::Line);
    EXPECT_EQ(exact.line.size(), 100u);
    EXPECT_EQ(lineOf(100, 99).status, Session::ReadStatus::TooLong);

    // Far beyond what one read delivers: still stopped at the limit
    const auto huge = lineOf(300 * 1024, 1000);
    EXPECT_EQ(huge.status, Session::ReadStatus::TooLong);
    EXPECT_TRUE(huge.line.empty());
}

TEST(processSession, closeStdinEndsTheChild) {
    Session session;
    startChild(session, {"echo"});
    EXPECT_EQ(ask(session, "x"), "echo:x");

    // Alive while stdin is open
    EXPECT_FALSE(session.wait(within(milliseconds(100))));
    EXPECT_FALSE(session.finished());

    session.closeStdin();
    session.closeStdin();  // idempotent
    EXPECT_EQ(session.writeLine("late", patiently()), Session::WriteStatus::Closed);
    ASSERT_TRUE(session.wait(patiently()));
    EXPECT_TRUE(session.finished());
    EXPECT_TRUE(session.exitStatus().exited);
    EXPECT_EQ(session.exitStatus().exitCode, 0);
    EXPECT_EQ(session.describeExit(), "exited with code 0");

    // The end of the child's output is reported as such
    EXPECT_EQ(session.readLine(patiently(), 1024).status, Session::ReadStatus::Eof);
}

TEST(processSession, killReportsHowTheChildEnded) {
    Session session;
    startChild(session, {"echo"});
    EXPECT_EQ(ask(session, "x"), "echo:x");

    session.kill();
    ASSERT_TRUE(session.finished());
    EXPECT_FALSE(session.exitStatus().exited);
#ifndef _WIN32
    EXPECT_EQ(session.exitStatus().termSignal, SIGKILL);
    EXPECT_NE(session.describeExit().find("signal 9"), std::string::npos) << session.describeExit();
#endif
    session.kill();  // nothing left to kill
    EXPECT_TRUE(session.wait(within(milliseconds(0))));
}

TEST(processSession, exitCodeAndStderrDiagnosticAreKept) {
    Session session;
    startChild(session, {"fail-stderr", "3"});
    ASSERT_TRUE(session.wait(patiently()));

    EXPECT_TRUE(session.exitStatus().exited);
    EXPECT_EQ(session.exitStatus().exitCode, 3);
    EXPECT_EQ(session.describeExit(), "exited with code 3");
    EXPECT_EQ(session.stderrTail(), "boom\n");
}

TEST(processSession, memoryLimitFailsAChildThatNeedsMore) {
    SKIP_UNDER_SANITIZER();
    const auto exitOfAllocation = [](const char* mib, std::uint64_t limit) {
        process::SessionOptions options;
        options.maxMemoryBytes = limit;
        Session session;
        startChild(session, {"allocate", mib}, options);
        EXPECT_TRUE(session.wait(patiently()));
        return std::make_pair(session.exitStatus(), session.stderrTail());
    };
    constexpr std::uint64_t kLimit = 256ull * 1024 * 1024;

    const auto unlimited = exitOfAllocation("1024", 0);
    EXPECT_TRUE(unlimited.first.exited);
    EXPECT_EQ(unlimited.first.exitCode, 0) << unlimited.second;

    const auto within = exitOfAllocation("64", kLimit);
    EXPECT_EQ(within.first.exitCode, 0) << within.second;

    const auto beyond = exitOfAllocation("1024", kLimit);
    EXPECT_NE(beyond.first.exitCode, 0);
    EXPECT_NE(beyond.second.find("out of memory"), std::string::npos) << beyond.second;
}

TEST(processSession, writeToAnExitedChildFailsAndTheHostSurvives) {
#ifndef _WIN32
    // The default SIGPIPE action would end this process if the session wrote with write()
    const DefaultSigpipe defaultSigpipe;
#endif
    Session session;
    startChild(session, {"exit", "0"});
    ASSERT_TRUE(session.wait(patiently()));

    EXPECT_EQ(session.writeLine("anyone there?", patiently()), Session::WriteStatus::Closed);
    EXPECT_EQ(session.writeLine(std::string(1024 * 1024, 'x'), patiently()), Session::WriteStatus::Closed);
    // Reaching this line is the check: the process was not killed by SIGPIPE
    EXPECT_TRUE(session.finished());
}

TEST(processSession, stderrFloodDoesNotBlockTheAnswers) {
    constexpr size_t kFloodBytes = 1024 * 1024;
    constexpr size_t kLineBytes = 64;
    size_t lines = 0;
    process::SessionOptions options;
    options.stderrCapacity = 4096;
    options.onStderrLine = [&lines](const std::string&) { ++lines; };

    Session session;
    startChild(session, {"flood-echo", std::to_string(kFloodBytes)}, options);

    // The child cannot answer before its whole 1 MiB of stderr is read
    ASSERT_EQ(session.writeLine("go", patiently()), Session::WriteStatus::Ok);
    const auto reply = session.readLine(Session::Clock::now() + seconds(15), 1024);
    ASSERT_EQ(reply.status, Session::ReadStatus::Line) << "the child is blocked on its stderr";
    EXPECT_EQ(reply.line, "echo:go");

    finishNormally(session);
    EXPECT_EQ(lines, kFloodBytes / kLineBytes);

    // Only the most recent bytes are kept
    const std::string tail = session.stderrTail();
    EXPECT_LE(tail.size(), options.stderrCapacity);
    EXPECT_NE(tail.find("flood-line-" + std::to_string(kFloodBytes / kLineBytes - 1)), std::string::npos);
    EXPECT_EQ(tail.find("flood-line-0 "), std::string::npos);
}

TEST(processSession, stderrIsDrainedWhileWaitingForTheExit) {
    constexpr size_t kFloodBytes = 1024 * 1024;
    Session session;
    startChild(session, {"flood-echo", std::to_string(kFloodBytes)});

    // The request is written and the answer never read: only wait() runs while the child
    // pours out its stderr, and it must keep up with it
    ASSERT_EQ(session.writeLine("go", patiently()), Session::WriteStatus::Ok);
    session.closeStdin();
    ASSERT_TRUE(session.wait(Session::Clock::now() + seconds(15))) << "the child is blocked on its stderr";
    EXPECT_EQ(session.exitStatus().exitCode, 0);
}

TEST(processSession, aWriteMeetsItsDeadlineWhileTheChildFloodsStderrAndReadsNothing) {
    // The child never reads its stdin and keeps its stderr readable at every moment, for longer
    // than the test waits (the host handles each line it drains, so the child writes faster than
    // the host reads): the write can only end with its own deadline
    constexpr milliseconds kDeadline(300);
    size_t lines = 0;
    process::SessionOptions options;
    options.onStderrLine = [&lines](const std::string&) { ++lines; };
    Session session;
    startChild(session, {"flood-stderr", "20000"}, options);

    const std::string request(4 * 1024 * 1024, 'w');  // more than any socket buffer holds
    const auto begin = Session::Clock::now();
    const auto status = session.write(request, begin + kDeadline);
    const auto took = Session::Clock::now() - begin;

    EXPECT_EQ(status, Session::WriteStatus::Timeout);
    EXPECT_LT(took, kDeadline + seconds(2)) << "the write went on past its deadline";
    EXPECT_GT(lines, 0u) << "stderr was drained while the write waited";
    session.kill();
}

TEST(processSession, environmentOverridesReachTheChild) {
    ASSERT_EQ(std::getenv("DDB_SESSION_TEST_VALUE"), nullptr);
    process::SessionOptions options;
    options.envOverrides = {{"DDB_SESSION_TEST_VALUE", "from the host"}};

    Session session;
    startChild(session, {"print-env", "DDB_SESSION_TEST_VALUE"}, options);
    const auto reply = session.readLine(patiently(), 1024);
    EXPECT_EQ(reply.status, Session::ReadStatus::Line);
    EXPECT_EQ(reply.line, "from the host");
    ASSERT_TRUE(session.wait(patiently()));

    // The host's own environment is not touched
    EXPECT_EQ(std::getenv("DDB_SESSION_TEST_VALUE"), nullptr);

    Session plain;
    startChild(plain, {"print-env", "DDB_SESSION_TEST_VALUE"});
    EXPECT_EQ(plain.readLine(patiently(), 1024).line, "<unset>");
    ASSERT_TRUE(plain.wait(patiently()));
}

TEST(processSession, missingBinaryReportsAStartError) {
    Session session;
    const fs::path missing = testChildPath().parent_path() / "ddbtest_no_such_helper";
    EXPECT_FALSE(session.start(missing, {}));
    EXPECT_FALSE(session.startError().empty());
    EXPECT_FALSE(session.started());
    EXPECT_EQ(session.pid(), 0u);

    // An unstarted session answers every call without a child
    EXPECT_EQ(session.writeLine("x", within(milliseconds(10))), Session::WriteStatus::Closed);
    EXPECT_EQ(session.readLine(within(milliseconds(10)), 10).status, Session::ReadStatus::Eof);
    EXPECT_FALSE(session.wait(within(milliseconds(10))));
    session.kill();
    session.closeStdin();

    // The error does not stop a later start of the same object
    EXPECT_TRUE(session.start(testChildPath(), {"echo"})) << session.startError();
    EXPECT_FALSE(session.start(testChildPath(), {"echo"}));  // a session starts once
    EXPECT_EQ(ask(session, "ok"), "echo:ok");
    finishNormally(session);
}

TEST(processSession, aMovedSessionKeepsItsChild) {
    Session first;
    startChild(first, {"echo"});
    Session second(std::move(first));
    EXPECT_FALSE(first.started());
    EXPECT_EQ(ask(second, "moved"), "echo:moved");
    finishNormally(second);
}

TEST(processSession, manySessionsAliveTogetherAllEndOnCloseStdin) {
    // Eight helpers started at the same moment from eight threads, then all stdins closed: a
    // child holding a copy of a sibling's stdin would never see EOF and wait() would time out
    constexpr int kSessions = 8;
    std::vector<Session> sessions(kSessions);
    std::atomic<bool> go{false};
    std::atomic<int> started{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < kSessions; ++i) {
        threads.emplace_back([&, i] {
            while (!go.load()) std::this_thread::yield();
            if (sessions[i].start(testChildPath(), {"echo"})) ++started;
        });
    }
    go = true;
    for (auto& t : threads) t.join();
    ASSERT_EQ(started.load(), kSessions);

    for (int i = 0; i < kSessions; ++i) EXPECT_EQ(ask(sessions[i], "n" + std::to_string(i)), "echo:n" + std::to_string(i));
    for (auto& s : sessions) s.closeStdin();
    for (auto& s : sessions) {
        ASSERT_TRUE(s.wait(patiently())) << "a helper never saw the end of its stdin";
        EXPECT_EQ(s.exitStatus().exitCode, 0);
    }
}

TEST(processSession, aSessionOutlivesTheThreadThatStartedIt) {
    // Hosts spawn from pool threads that come and go: the child must not die with its spawner
    Session session;
    bool started = false;
    std::thread([&] { started = session.start(testChildPath(), {"echo"}); }).join();
    ASSERT_TRUE(started) << session.startError();

    // A child killed with its spawning thread would be gone within moments: the end of its
    // output would show here instead of a quiet timeout
    EXPECT_EQ(session.readLine(within(milliseconds(300)), 1024).status, Session::ReadStatus::Timeout);
    EXPECT_FALSE(session.finished());
    EXPECT_EQ(ask(session, "still here"), "echo:still here");
    finishNormally(session);
}

#ifdef __linux__
namespace {
// The open descriptors, except the listing's own (its number is reused by whatever opens next)
std::set<int> openDescriptors() {
    std::set<int> fds;
    for (const auto& entry : fs::directory_iterator("/proc/self/fd")) {
        std::error_code ec;
        const std::string target = fs::read_symlink(entry.path(), ec).string();
        if (!ec && target.rfind("/proc/", 0) != 0) fds.insert(std::stoi(entry.path().filename().string()));
    }
    return fds;
}

// The flags line of /proc/self/fdinfo/<fd> (octal), or -1 when the descriptor is gone
long descriptorFlags(int fd) {
    std::ifstream in("/proc/self/fdinfo/" + std::to_string(fd));
    std::string key;
    while (in >> key) {
        if (key == "flags:") {
            long flags = -1;
            in >> std::oct >> flags;
            return flags;
        }
    }
    return -1;
}
}  // namespace

TEST(processSession, everyDescriptorTheHostKeepsIsCloseOnExec) {
    const std::set<int> before = openDescriptors();
    Session session;
    startChild(session, {"echo"});
    const std::set<int> after = openDescriptors();

    // stdin, stdout, stderr (and the process handle where the kernel has one)
    int checked = 0;
    for (const int fd : after) {
        if (before.count(fd)) continue;
        const long flags = descriptorFlags(fd);
        ASSERT_GE(flags, 0) << "descriptor " << fd;
        EXPECT_NE(flags & O_CLOEXEC, 0) << "descriptor " << fd << " would be inherited by the next child";
        ++checked;
    }
    EXPECT_GE(checked, 3);

    session.kill();
}

TEST(processSession, destructorKillsAndReapsTheChild) {
    unsigned long pid = 0;
    {
        Session session;
        startChild(session, {"echo"});
        pid = session.pid();
        ASSERT_GT(pid, 0u);
        EXPECT_EQ(::kill(static_cast<pid_t>(pid), 0), 0);
    }
    // Reaped, not left as a zombie: the pid no longer exists
    EXPECT_EQ(::kill(static_cast<pid_t>(pid), 0), -1);
    EXPECT_EQ(errno, ESRCH);
}
#endif
