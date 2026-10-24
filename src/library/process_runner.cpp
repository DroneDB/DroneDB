/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "process_runner.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

#include "cancellation.h"
#include "logger.h"
#include "unique_handle.h"
#include "mio.h"

#ifdef _WIN32
#include <cwchar>
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif
extern char **environ;
#endif

namespace ddb
{
    namespace process
    {

        namespace
        {

            using Clock = std::chrono::steady_clock;
            using std::chrono::milliseconds;

            // Size of a single read from a child pipe.
            constexpr size_t kReadChunk = 64 * 1024;

            // A line longer than this is delivered in pieces to onOutputLine.
            constexpr size_t kMaxLineLength = 64 * 1024;

            // Poll interval for the child's exit once every monitored pipe is closed.
            constexpr long long kExitPollMs = 50;

            // Bounded buffer keeping only the most recent `capacity` bytes. Trimming
            // is amortised: the backing string may grow to 2x capacity before the
            // oldest bytes are dropped.
            class RingBuffer
            {
              public:
                explicit RingBuffer(size_t capacity) : capacity_(capacity > 0 ? capacity : 1) {}

                void append(const char *data, size_t n)
                {
                    if (n >= capacity_)
                    {
                        buf_.assign(data + (n - capacity_), capacity_);
                        return;
                    }
                    buf_.append(data, n);
                    if (buf_.size() > 2 * capacity_)
                        buf_.erase(0, buf_.size() - capacity_);
                }

                std::string str() const
                {
                    if (buf_.size() <= capacity_) return buf_;
                    return buf_.substr(buf_.size() - capacity_);
                }

              private:
                size_t capacity_;
                std::string buf_;
            };

            // Collects stdout/stderr: ring buffer, byte count and line callback.
            class OutputSink
            {
              public:
                explicit OutputSink(const Options &opts)
                    : ring_(opts.outputCapacity), onLine_(opts.onOutputLine) {}

                void feed(const char *data, size_t n)
                {
                    total_ += n;
                    ring_.append(data, n);
                    if (!onLine_) return;
                    for (size_t i = 0; i < n; ++i)
                    {
                        const char c = data[i];
                        if (c == '\n' || c == '\r')
                        {
                            flush();
                        }
                        else
                        {
                            partial_.push_back(c);
                            if (partial_.size() >= kMaxLineLength) flush();
                        }
                    }
                }

                void flush()
                {
                    if (partial_.empty() || !onLine_) return;
                    try
                    {
                        onLine_(partial_);
                    }
                    catch (...)
                    {
                        // A logging callback must never abort the supervision loop.
                    }
                    partial_.clear();
                }

                void finish(Result &r)
                {
                    flush();
                    r.outputTail = ring_.str();
                    r.totalBytes = total_;
                }

              private:
                RingBuffer ring_;
                std::function<void(const std::string &)> onLine_;
                std::string partial_;
                uint64_t total_ = 0;
            };

            void deliverExtra(const Options &opts, const char *data, size_t n)
            {
                if (!opts.onExtraData) return;
                try
                {
                    opts.onExtraData(data, n);
                }
                catch (...)
                {
                    // Never let a consumer abort the supervision loop.
                }
            }

            std::string fmtSeconds(milliseconds ms)
            {
                std::ostringstream oss;
                oss.imbue(std::locale::classic());
                if (ms.count() < 10000)
                    oss << std::fixed << std::setprecision(1) << (static_cast<double>(ms.count()) / 1000.0) << " s";
                else
                    oss << (ms.count() / 1000) << " s";
                return oss.str();
            }

            std::string timeoutMessage(milliseconds idle)
            {
                return "killed after " + fmtSeconds(idle) + " without output";
            }

            std::string runtimeMessage(milliseconds limit)
            {
                return "killed after running for " + fmtSeconds(limit) + " (time limit)";
            }

            // Milliseconds left before the first enabled limit (idle timeout measured
            // from lastActivity, maxRuntime from start) expires; sets runtimeHit when
            // maxRuntime is the one that expires first. -1 when no limit is enabled.
            template <typename TimePoint>
            long long remainingBudget(const Options &opts, TimePoint start, TimePoint lastActivity,
                                      TimePoint now, bool &runtimeHit)
            {
                bool limited = false;
                long long remaining = -1;
                runtimeHit = false;
                if (opts.idleTimeout.count() > 0)
                {
                    remaining = std::chrono::duration_cast<milliseconds>(lastActivity + opts.idleTimeout - now).count();
                    limited = true;
                }
                if (opts.maxRuntime.count() > 0)
                {
                    const long long left =
                        std::chrono::duration_cast<milliseconds>(start + opts.maxRuntime - now).count();
                    if (!limited || left < remaining)
                    {
                        remaining = left;
                        runtimeHit = true;
                    }
                    limited = true;
                }
                return limited ? remaining : -1;
            }

            bool anyLimit(const Options &opts)
            {
                return opts.idleTimeout.count() > 0 || opts.maxRuntime.count() > 0;
            }

            std::string limitMessage(const Options &opts, bool runtimeHit)
            {
                return runtimeHit ? runtimeMessage(opts.maxRuntime) : timeoutMessage(opts.idleTimeout);
            }

            // True when a limit has run out at `now`: r then says which one. Otherwise `remaining`
            // is the time left before the first one. Only for anyLimit(opts).
            template <typename TimePoint>
            bool limitExpired(const Options &opts, Result &r, TimePoint start, TimePoint lastActivity,
                              TimePoint now, long long &remaining)
            {
                bool runtimeHit = false;
                remaining = remainingBudget(opts, start, lastActivity, now, runtimeHit);
                if (remaining > 0) return false;
                r.timedOut = true;
                r.errorMessage = limitMessage(opts, runtimeHit);
                return true;
            }

            // The work of the calling thread was canceled (cancellation.h): the child is killed
            void markCanceled(Result &r)
            {
                r.canceled = true;
                r.errorMessage = "killed: the call was canceled";
            }

            std::string quoteForLog(const std::string &arg)
            {
#ifdef _WIN32
                // CommandLineToArgvW / MSVC CRT quoting rules.
                if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos)
                    return arg;

                std::string out;
                out.reserve(arg.size() + 2);
                out.push_back('"');
                size_t backslashes = 0;
                for (char c : arg)
                {
                    if (c == '\\')
                    {
                        ++backslashes;
                        out.push_back(c);
                    }
                    else if (c == '"')
                    {
                        // Escape the preceding backslashes and the quote itself.
                        out.append(backslashes + 1, '\\');
                        out.push_back('"');
                        backslashes = 0;
                    }
                    else
                    {
                        backslashes = 0;
                        out.push_back(c);
                    }
                }
                // Trailing backslashes are doubled before the closing quote.
                out.append(backslashes, '\\');
                out.push_back('"');
                return out;
#else
                if (!arg.empty() && arg.find_first_of(" \t\n'\"\\$`*?;&|<>()") == std::string::npos)
                    return arg;
                std::string out = "'";
                for (char c : arg)
                {
                    if (c == '\'')
                        out += "'\\''";
                    else
                        out.push_back(c);
                }
                out.push_back('\'');
                return out;
#endif
            }

            std::vector<std::string> expandArgs(const std::vector<std::string> &args,
                                                const Options &opts,
                                                const std::string &extraId)
            {
                std::vector<std::string> out;
                out.reserve(args.size());
                for (const auto &a : args)
                {
                    if (opts.extraChannel && a == kExtraChannelPlaceholder)
                        out.push_back(extraId);
                    else
                        out.push_back(a);
                }
                return out;
            }

            // The binary and its arguments as one quoted command line
            std::string quotedCommandLine(const fs::path &binary, const std::vector<std::string> &args)
            {
                std::string out = quoteForLog(io::toUtf8(binary));
                for (const auto &a : args)
                {
                    out.push_back(' ');
                    out.append(quoteForLog(a));
                }
                return out;
            }

#ifndef _WIN32

            // ---------------------------------------------------------------- POSIX

            // pipe with FD_CLOEXEC on both ends (atomic where pipe2 exists); ends[0] is the
            // read end, ends[1] the write end.
            bool makePipe(UniqueFd (&ends)[2])
            {
                int fds[2];
#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
                if (::pipe2(fds, O_CLOEXEC) != 0) return false;
#else
                if (::pipe(fds) != 0) return false;
                ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
                ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#endif
                ends[0].reset(fds[0]);
                ends[1].reset(fds[1]);
                return true;
            }

            // Everything the child needs, prepared before fork().
            struct ChildPlan
            {
                const char *path = nullptr;
                char *const *argv = nullptr;
                char *const *envp = nullptr;
                int devNull = -1;
                int outWrite = -1;
                int extraWrite = -1; // -1 when no extra channel
                int statusWrite = -1;
                long maxFd = 1024;  // upper bound for the close() fallback loop
                pid_t parentPid = 0;
            };

            // Runs in the forked child: only async-signal-safe calls from here on.
            [[noreturn]] void childExec(const ChildPlan &p)
            {
                int statusFd = p.statusWrite;
                auto fail = [&statusFd](int err) {
                    ssize_t ignored = ::write(statusFd, &err, sizeof(err));
                    (void)ignored;
                    ::_exit(127);
                };

#ifdef __linux__
                // Die with the spawning thread; then close the race where the parent
                // died before prctl() took effect.
                ::prctl(PR_SET_PDEATHSIG, SIGKILL);
                if (::getppid() != p.parentPid) ::_exit(127);
#endif
                // Own process group so a timeout can kill the whole tree.
                ::setpgid(0, 0);

                // Do not leak the parent's blocked signals or ignored SIGPIPE.
                sigset_t empty;
                sigemptyset(&empty);
                ::sigprocmask(SIG_SETMASK, &empty, nullptr);
                struct sigaction dfl;
                std::memset(&dfl, 0, sizeof(dfl));
                dfl.sa_handler = SIG_DFL;
                sigemptyset(&dfl.sa_mask);
                ::sigaction(SIGPIPE, &dfl, nullptr);

                // Move every source descriptor above the target range first so the
                // dup2() calls below can never clobber one another (e.g. when the
                // parent had fds 0-2 closed and a pipe landed on them).
                constexpr int kLow = 10;
                const int devNull = ::fcntl(p.devNull, F_DUPFD, kLow);
                if (devNull < 0) fail(errno);
                const int outW = ::fcntl(p.outWrite, F_DUPFD, kLow);
                if (outW < 0) fail(errno);
                int extraW = -1;
                if (p.extraWrite >= 0)
                {
                    extraW = ::fcntl(p.extraWrite, F_DUPFD, kLow);
                    if (extraW < 0) fail(errno);
                }
                const int statusTarget = p.extraWrite >= 0 ? kExtraChannelFd + 1 : kExtraChannelFd;
                const int statusW = ::fcntl(p.statusWrite, F_DUPFD, kLow);
                if (statusW < 0) fail(errno);

                // dup2 creates the targets without FD_CLOEXEC (inherited by exec).
                if (::dup2(devNull, STDIN_FILENO) < 0) fail(errno);
                if (::dup2(outW, STDOUT_FILENO) < 0) fail(errno);
                if (::dup2(outW, STDERR_FILENO) < 0) fail(errno);
                if (extraW >= 0 && ::dup2(extraW, kExtraChannelFd) < 0) fail(errno);
                if (::dup2(statusW, statusTarget) < 0) fail(errno);
                statusFd = statusTarget;
                // The status pipe must close on a successful exec.
                if (::fcntl(statusTarget, F_SETFD, FD_CLOEXEC) < 0) fail(errno);

                // Close every other descriptor (including the temporaries above).
                const int firstToClose = statusTarget + 1;
                bool closed = false;
#if defined(__linux__) && defined(SYS_close_range)
                closed = ::syscall(SYS_close_range, static_cast<unsigned int>(firstToClose), ~0U, 0U) == 0;
#endif
                if (!closed)
                {
                    for (long fd = firstToClose; fd < p.maxFd; ++fd)
                        ::close(static_cast<int>(fd));
                }

                ::execve(p.path, p.argv, p.envp);
                fail(errno);
                ::_exit(127); // unreachable
            }

            long computeMaxFd()
            {
                constexpr long kCap = 1L << 20;
                long maxFd = -1;
                struct rlimit rl;
                if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
                    maxFd = static_cast<long>(rl.rlim_cur);
                if (maxFd <= 0) maxFd = ::sysconf(_SC_OPEN_MAX);
                if (maxFd <= 0 || maxFd > kCap) maxFd = kCap;
                return maxFd;
            }

            void killGroup(pid_t pid)
            {
                ::kill(-pid, SIGKILL);
                ::kill(pid, SIGKILL); // in case setpgid did not take effect
            }

            // waitpid with EINTR retry. Returns false on unexpected failure.
            bool waitChild(pid_t pid, int &status)
            {
                for (;;)
                {
                    pid_t w = ::waitpid(pid, &status, 0);
                    if (w == pid) return true;
                    if (w < 0 && errno == EINTR) continue;
                    return false;
                }
            }

            // The pointers execve takes (char *const[], null-terminated) over strings the vector
            // owns: data() of a non-const string, so nothing is cast away.
            std::vector<char *> cStringPointers(std::vector<std::string> &strings)
            {
                std::vector<char *> out;
                out.reserve(strings.size() + 1);
                for (auto &s : strings) out.push_back(s.data());
                out.push_back(nullptr);
                return out;
            }

            // The parent's environment with the overrides applied (the parent's own is never
            // modified).
            std::vector<std::string> childEnvironment(const Options &opts)
            {
                std::vector<std::string> envStrings;
                for (char **e = environ; e && *e; ++e)
                {
                    const std::string entry(*e);
                    const std::string name = entry.substr(0, entry.find('='));
                    bool overridden = false;
                    for (const auto &ov : opts.envOverrides)
                    {
                        if (ov.first == name)
                        {
                            overridden = true;
                            break;
                        }
                    }
                    if (!overridden) envStrings.push_back(entry);
                }
                for (const auto &ov : opts.envOverrides)
                    envStrings.push_back(ov.first + "=" + ov.second);
                return envStrings;
            }

            // One child process, from fork() to waitpid(): launch() sets it up, drainPipes()
            // reads its output while the limits are enforced, awaitExit() covers a child that
            // closed its pipes and runs on, collectStatus() reaps it.
            class PosixRun
            {
              public:
                PosixRun(const fs::path &binary, const std::vector<std::string> &args, const Options &opts)
                    : opts_(opts), start_(Clock::now()), sink_(opts), path_(io::toUtf8(binary)),
                      buf_(kReadChunk)
                {
                    argStrings_.reserve(args.size() + 1);
                    argStrings_.push_back(path_);
                    for (auto &a : expandArgs(args, opts, std::to_string(kExtraChannelFd)))
                        argStrings_.push_back(std::move(a));
                    envStrings_ = childEnvironment(opts);
                }

                Result run()
                {
                    if (!launch()) return finish();
                    drainPipes();
                    if (!killed_ && !childExited_) awaitExit();
                    collectStatus();
                    return finish();
                }

              private:
                // Records why the child could not be set up; errno is the reason.
                bool setupFailure(const char *what)
                {
                    const int err = errno;
                    r_.errorMessage = std::string(what) + ": " + std::strerror(err);
                    return false;
                }

                Result finish()
                {
                    r_.duration = std::chrono::duration_cast<milliseconds>(Clock::now() - start_);
                    sink_.finish(r_);
                    return r_;
                }

                // Forks the child and reads its exec status. False: r_ says what failed.
                bool launch()
                {
                    std::vector<char *> argv = cStringPointers(argStrings_);
                    std::vector<char *> envp = cStringPointers(envStrings_);

                    if (!makePipe(outPipe_)) return setupFailure("pipe() for output");
                    if (opts_.extraChannel && !makePipe(extraPipe_))
                        return setupFailure("pipe() for extra channel");
                    if (!makePipe(statusPipe_)) return setupFailure("pipe() for exec status");
                    devNull_.reset(::open("/dev/null", O_RDONLY | O_CLOEXEC));
                    if (!devNull_) return setupFailure("open(/dev/null)");

                    ChildPlan plan;
                    plan.path = path_.c_str();
                    plan.argv = argv.data();
                    plan.envp = envp.data();
                    plan.devNull = devNull_.get();
                    plan.outWrite = outPipe_[1].get();
                    plan.extraWrite = extraPipe_[1].get();
                    plan.statusWrite = statusPipe_[1].get();
                    plan.maxFd = computeMaxFd();
                    plan.parentPid = ::getpid();

                    pid_ = ::fork();
                    if (pid_ < 0) return setupFailure("fork()");
                    if (pid_ == 0) childExec(plan);

                    ::setpgid(pid_, pid_); // mirror the child's call (avoids a kill(-pid) race)
                    outPipe_[1].reset();
                    extraPipe_[1].reset();
                    statusPipe_[1].reset();
                    devNull_.reset();

                    // Exec status: EOF means execve succeeded (CLOEXEC), an int is errno.
                    int execErr = 0;
                    ssize_t sn;
                    do
                    {
                        sn = ::read(statusPipe_[0].get(), &execErr, sizeof(execErr));
                    } while (sn < 0 && errno == EINTR);
                    statusPipe_[0].reset();

                    if (sn == static_cast<ssize_t>(sizeof(execErr)))
                    {
                        int status = 0;
                        waitChild(pid_, status);
                        r_.errorMessage = std::strerror(execErr);
                        return false;
                    }
                    r_.started = true;
                    return true;
                }

                // Kills the child's group and stops the supervision.
                void killChild()
                {
                    killGroup(pid_);
                    killed_ = true;
                }

                // Observe exit before applying the idle timeout. Once the direct child exits,
                // only the drain deadline applies. WNOWAIT keeps the pid/group reserved for
                // killGroup().
                void probeExit()
                {
                    siginfo_t si{};
                    if (::waitid(P_PID, static_cast<id_t>(pid_), &si, WEXITED | WNOHANG | WNOWAIT) == 0 &&
                        si.si_pid == pid_)
                    {
                        childExited_ = true;
                        exitTime_ = Clock::now();
                    }
                }

                // Drains both pipes until EOF, the child's exit plus the drain grace, a limit,
                // or the cancellation.
                void drainPipes()
                {
                    lastActivity_ = Clock::now();
                    while (outPipe_[0] || extraPipe_[0])
                    {
                        if (!childExited_) probeExit();
                        if (!childExited_ && cancel::isCanceled())
                        {
                            markCanceled(r_);
                            killChild();
                            break;
                        }
                        const auto now = Clock::now();
                        long long waitMs = 500; // periodic check for child exit and cancellation
                        if (!childExited_ && anyLimit(opts_))
                        {
                            long long remaining = 0;
                            if (limitExpired(opts_, r_, start_, lastActivity_, now, remaining))
                            {
                                killChild();
                                break;
                            }
                            waitMs = std::min(waitMs, remaining);
                        }
                        if (childExited_)
                        {
                            const long long left = static_cast<long long>(
                                std::chrono::duration_cast<milliseconds>(exitTime_ + kExitDrainGrace - now).count());
                            if (left <= 0)
                            {
                                // The child is gone but a descendant keeps the pipes open.
                                r_.descendantsTerminated = true;
                                killChild();
                                break;
                            }
                            waitMs = std::min(waitMs, left);
                        }

                        pollfd pfds[2];
                        int fdKinds[2];
                        nfds_t n = 0;
                        if (outPipe_[0])
                        {
                            pfds[n] = {outPipe_[0].get(), POLLIN, 0};
                            fdKinds[n++] = 0;
                        }
                        if (extraPipe_[0])
                        {
                            pfds[n] = {extraPipe_[0].get(), POLLIN, 0};
                            fdKinds[n++] = 1;
                        }

                        const int pr = ::poll(pfds, n, static_cast<int>(std::max<long long>(waitMs, 0LL)));
                        if (pr < 0)
                        {
                            if (errno == EINTR) continue;
                            r_.errorMessage = std::string("poll() failed: ") + std::strerror(errno);
                            killChild();
                            break;
                        }

                        for (nfds_t i = 0; i < n; ++i)
                        {
                            if (pfds[i].revents == 0) continue;
                            readPipe(fdKinds[i] == 0 ? outPipe_[0] : extraPipe_[0], fdKinds[i] == 0);
                        }
                    }
                    outPipe_[0].reset();
                    extraPipe_[0].reset();
                }

                // Reads what is available on one pipe; bytes count as activity. The pipe is
                // closed on EOF or an unrecoverable error.
                void readPipe(UniqueFd &fd, bool isOutput)
                {
                    const ssize_t nr = ::read(fd.get(), buf_.data(), buf_.size());
                    if (nr > 0)
                    {
                        lastActivity_ = Clock::now();
                        if (isOutput)
                            sink_.feed(buf_.data(), static_cast<size_t>(nr));
                        else
                            deliverExtra(opts_, buf_.data(), static_cast<size_t>(nr));
                    }
                    else if (nr == 0 || (errno != EINTR && errno != EAGAIN))
                    {
                        fd.reset(); // EOF or unrecoverable error
                    }
                }

                // Every monitored pipe is closed but the child may still be running
                // (e.g. it closed stdout/stderr itself): keep enforcing the idle
                // timeout, measured from the last activity, and the cancellation instead
                // of blocking in waitpid() forever. Nothing can count as activity any more.
                void awaitExit()
                {
                    for (;;)
                    {
                        siginfo_t si;
                        std::memset(&si, 0, sizeof(si));
                        const int wr = ::waitid(P_PID, static_cast<id_t>(pid_), &si, WEXITED | WNOHANG | WNOWAIT);
                        if (wr == 0 && si.si_pid == pid_) break; // exited (not reaped yet)
                        if (wr < 0 && errno != EINTR) break;     // waitChild() reports it

                        if (cancel::isCanceled())
                        {
                            markCanceled(r_);
                            killGroup(pid_);
                            break;
                        }
                        long long pollMs = kExitPollMs;
                        if (anyLimit(opts_))
                        {
                            long long remaining = 0;
                            if (limitExpired(opts_, r_, start_, lastActivity_, Clock::now(), remaining))
                            {
                                killGroup(pid_);
                                break;
                            }
                            pollMs = std::min<long long>(remaining, kExitPollMs);
                        }
                        ::poll(nullptr, 0, static_cast<int>(pollMs));
                    }
                }

                // Reaps the child and records how it ended.
                void collectStatus()
                {
                    int status = 0;
                    if (!waitChild(pid_, status))
                    {
                        if (r_.errorMessage.empty())
                            r_.errorMessage = std::string("waitpid() failed: ") + std::strerror(errno);
                    }
                    else if (WIFEXITED(status))
                    {
                        r_.exited = true;
                        r_.exitCode = WEXITSTATUS(status);
                    }
                    else if (WIFSIGNALED(status))
                    {
                        r_.termSignal = WTERMSIG(status);
                    }
                }

                const Options &opts_;
                const Clock::time_point start_;
                OutputSink sink_;
                Result r_;
                const std::string path_;
                std::vector<std::string> argStrings_;
                std::vector<std::string> envStrings_;
                std::vector<char> buf_;

                // Every descriptor of the parent closes on return (and on an exception)
                UniqueFd outPipe_[2];
                UniqueFd extraPipe_[2];
                UniqueFd statusPipe_[2];
                UniqueFd devNull_;

                pid_t pid_ = 0;
                bool childExited_ = false;
                bool killed_ = false;
                Clock::time_point exitTime_;
                Clock::time_point lastActivity_;
            };

            Result runPosix(const fs::path &binary, const std::vector<std::string> &args, const Options &opts)
            {
                return PosixRun(binary, args, opts).run();
            }

#else

            // -------------------------------------------------------------- Windows

            std::string winErrorMessage(DWORD err)
            {
                LPWSTR msg = nullptr;
                const DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                                     FORMAT_MESSAGE_IGNORE_INSERTS,
                                                 nullptr, err, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
                std::string text;
                if (len > 0 && msg)
                {
                    text = io::fromWide(std::wstring_view(msg, len));
                    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' ||
                                             text.back() == '.'))
                        text.pop_back();
                }
                if (msg) LocalFree(msg);
                std::ostringstream oss;
                oss << (text.empty() ? "Windows error" : text) << " (error " << err << ")";
                return oss.str();
            }

            std::wstring envName(const std::wstring &entry)
            {
                // Entries such as "=C:=C:\dir" start with '='; the name ends at the
                // first '=' after position 0.
                const size_t eq = entry.find(L'=', 1);
                return eq == std::wstring::npos ? entry : entry.substr(0, eq);
            }

            int compareNames(const std::wstring &a, const std::wstring &b)
            {
                const int c = CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                                                   static_cast<int>(b.size()), TRUE);
                return c == CSTR_LESS_THAN ? -1 : (c == CSTR_EQUAL ? 0 : 1);
            }

            // UTF-16 environment block: the parent's environment with overrides
            // applied (case-insensitive names), sorted as Windows requires.
            std::vector<wchar_t> buildEnvBlock(const std::vector<std::pair<std::string, std::string>> &overrides)
            {
                std::vector<std::wstring> entries;
                LPWCH base = GetEnvironmentStringsW();
                if (base)
                {
                    for (LPWCH p = base; *p; p += wcslen(p) + 1)
                        entries.emplace_back(p);
                    FreeEnvironmentStringsW(base);
                }
                for (const auto &ov : overrides)
                {
                    const std::wstring name = io::toWide(ov.first);
                    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                                 [&](const std::wstring &e) { return compareNames(envName(e), name) == 0; }),
                                  entries.end());
                    entries.push_back(name + L"=" + io::toWide(ov.second));
                }
                std::stable_sort(entries.begin(), entries.end(), [](const std::wstring &a, const std::wstring &b) {
                    return compareNames(envName(a), envName(b)) < 0;
                });

                std::vector<wchar_t> block;
                for (const auto &e : entries)
                {
                    block.insert(block.end(), e.begin(), e.end());
                    block.push_back(L'\0');
                }
                if (block.empty()) block.push_back(L'\0');
                block.push_back(L'\0');
                return block;
            }

            // Kill-on-close job: the child (and its descendants) die with us. Empty when the job
            // cannot be made.
            UniqueHandle makeKillOnCloseJob()
            {
                UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
                if (job)
                {
                    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
                    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &jeli,
                                                 sizeof(jeli)))
                        job.reset();
                }
                return job;
            }

            // One child process, from CreateProcessW to its exit code: launch() sets it up,
            // pumpPipes() reads its output while the limits are enforced, awaitExit() covers a
            // child that closed its pipes and runs on, collectStatus() records how it ended.
            class WindowsRun
            {
              public:
                WindowsRun(const fs::path &binary, const std::vector<std::string> &args, const Options &opts)
                    : binary_(binary), args_(args), opts_(opts), start_(Clock::now()), sink_(opts),
                      buf_(kReadChunk)
                {
                }

                Result run()
                {
                    if (!launch()) return finish();
                    pumpPipes();
                    if (!r_.timedOut && !r_.canceled && !childExited_) awaitExit();
                    collectStatus();
                    return finish();
                }

              private:
                bool setupFailure(const std::string &what, DWORD err)
                {
                    r_.errorMessage = what + ": " + winErrorMessage(err);
                    return false;
                }

                Result finish()
                {
                    r_.duration = std::chrono::duration_cast<milliseconds>(Clock::now() - start_);
                    sink_.finish(r_);
                    return r_;
                }

                // False: r_ says what failed.
                bool launch()
                {
                    if (!createPipes()) return false;
                    PROCESS_INFORMATION pi{};
                    if (!startChild(pi)) return false;
                    adoptChild(pi);
                    return true;
                }

                // Pipes are created non-inheritable; only the child's ends are made
                // inheritable, and only the handles in the explicit list are passed.
                bool createPipes()
                {
                    if (!CreatePipe(outRead_.put(), outWrite_.put(), nullptr, 0))
                        return setupFailure("CreatePipe for output", GetLastError());
                    if (!SetHandleInformation(outWrite_.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
                        return setupFailure("SetHandleInformation", GetLastError());
                    if (opts_.extraChannel)
                    {
                        if (!CreatePipe(extraRead_.put(), extraWrite_.put(), nullptr, 0))
                            return setupFailure("CreatePipe for extra channel", GetLastError());
                        if (!SetHandleInformation(extraWrite_.get(), HANDLE_FLAG_INHERIT,
                                                  HANDLE_FLAG_INHERIT))
                            return setupFailure("SetHandleInformation", GetLastError());
                    }
                    SECURITY_ATTRIBUTES saInherit{};
                    saInherit.nLength = sizeof(saInherit);
                    saInherit.bInheritHandle = TRUE;
                    // INVALID_HANDLE_VALUE on failure, which UniqueHandle never closes
                    nul_.reset(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                           &saInherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
                    if (!nul_)
                        return setupFailure("open NUL", GetLastError());
                    return true;
                }

                // Creates the child suspended, inside the job; the parent lets go of the child's
                // ends of the pipes.
                bool startChild(PROCESS_INFORMATION &pi)
                {
                    std::vector<HANDLE> inherit = {nul_.get(), outWrite_.get()};
                    if (extraWrite_) inherit.push_back(extraWrite_.get());

                    SIZE_T attrSize = 0;
                    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
                    std::vector<unsigned char> attrBuf(attrSize);
                    auto attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
                    if (!InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize))
                        return setupFailure("InitializeProcThreadAttributeList", GetLastError());
                    if (!UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit.data(),
                                                   inherit.size() * sizeof(HANDLE), nullptr, nullptr))
                    {
                        const DWORD err = GetLastError();
                        DeleteProcThreadAttributeList(attrList);
                        return setupFailure("UpdateProcThreadAttribute", err);
                    }

                    STARTUPINFOEXW siex{};
                    siex.StartupInfo.cb = sizeof(siex);
                    siex.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
                    siex.StartupInfo.hStdInput = nul_.get();
                    siex.StartupInfo.hStdOutput = outWrite_.get();
                    siex.StartupInfo.hStdError = outWrite_.get();
                    siex.lpAttributeList = attrList;

                    // The child inherits the handle with the same value: pass it as size_t
                    // (QgisUntwine convention for --progress_fd).
                    const std::string extraId =
                        extraWrite_ ? std::to_string(reinterpret_cast<uintptr_t>(extraWrite_.get()))
                                    : std::string();
                    std::wstring wideCmd = io::toWide(quotedCommandLine(binary_, expandArgs(args_, opts_, extraId)));
                    wideCmd.push_back(L'\0'); // CreateProcessW may modify the buffer
                    const std::wstring wideBin = binary_.wstring();

                    std::vector<wchar_t> envBlock;
                    DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED;
                    if (!opts_.envOverrides.empty())
                    {
                        envBlock = buildEnvBlock(opts_.envOverrides);
                        flags |= CREATE_UNICODE_ENVIRONMENT;
                    }

                    job_ = makeKillOnCloseJob();

                    const BOOL ok = CreateProcessW(wideBin.c_str(), wideCmd.data(), nullptr, nullptr, TRUE, flags,
                                                   envBlock.empty() ? nullptr : envBlock.data(), nullptr,
                                                   &siex.StartupInfo, &pi);
                    const DWORD createErr = ok ? 0 : GetLastError();
                    DeleteProcThreadAttributeList(attrList);

                    // The parent never uses the child's ends.
                    outWrite_.reset();
                    extraWrite_.reset();
                    nul_.reset();

                    if (!ok)
                    {
                        r_.errorMessage = winErrorMessage(createErr);
                        return false;
                    }
                    return true;
                }

                // Puts the suspended child in the job and lets it run.
                void adoptChild(const PROCESS_INFORMATION &pi)
                {
                    childProcess_.reset(pi.hProcess);
                    UniqueHandle childThread(pi.hThread);
                    inJob_ = job_ && AssignProcessToJobObject(job_.get(), childProcess_.get());
                    if (!inJob_)
                        LOGD << "Could not assign child process to a job object (error " << GetLastError()
                             << "); orphan protection is unavailable";
                    ResumeThread(childThread.get());
                    childThread.reset();
                    r_.started = true;
                }

                void terminateChild()
                {
                    if (inJob_)
                        TerminateJobObject(job_.get(), 1);
                    else
                        TerminateProcess(childProcess_.get(), 1);
                }

                // Anonymous pipes do not support overlapped I/O: poll with PeekNamedPipe and wait
                // on the process handle while idle. True when bytes were read.
                bool pump(UniqueHandle &h, bool isOutput)
                {
                    if (!h) return false;
                    DWORD avail = 0;
                    if (!PeekNamedPipe(h.get(), nullptr, 0, nullptr, &avail, nullptr))
                    {
                        h.reset(); // ERROR_BROKEN_PIPE: writer closed and drained
                        return false;
                    }
                    if (avail == 0) return false;
                    DWORD nr = 0;
                    const DWORD want = static_cast<DWORD>(std::min<size_t>(avail, buf_.size()));
                    if (!ReadFile(h.get(), buf_.data(), want, &nr, nullptr) || nr == 0)
                    {
                        h.reset();
                        return false;
                    }
                    if (isOutput)
                        sink_.feed(buf_.data(), nr);
                    else
                        deliverExtra(opts_, buf_.data(), nr);
                    return true;
                }

                // Drains both pipes until EOF, the child's exit plus the drain grace, a limit,
                // or the cancellation.
                void pumpPipes()
                {
                    lastActivity_ = Clock::now();
                    while (outRead_ || extraRead_)
                    {
                        // Probe on every iteration, including when a descendant is
                        // continuously writing. Record the first observed exit once.
                        if (!childExited_ && WaitForSingleObject(childProcess_.get(), 0) == WAIT_OBJECT_0)
                        {
                            childExited_ = true;
                            exitTime_ = Clock::now();
                        }
                        if (!childExited_ && cancel::isCanceled())
                        {
                            markCanceled(r_);
                            terminateChild();
                            break;
                        }
                        bool active = pump(outRead_, true);
                        active = pump(extraRead_, false) || active;
                        const auto now = Clock::now();
                        if (active) lastActivity_ = now;
                        if (!childExited_ && anyLimit(opts_))
                        {
                            long long remaining = 0;
                            if (limitExpired(opts_, r_, start_, lastActivity_, now, remaining))
                            {
                                terminateChild();
                                break;
                            }
                        }
                        if (childExited_ && (outRead_ || extraRead_) && now - exitTime_ >= kExitDrainGrace)
                        {
                            terminateChild(); // a descendant keeps the pipes open
                            r_.descendantsTerminated = inJob_;
                            break;
                        }
                        if (!active) Sleep(25);
                    }
                    outRead_.reset();
                    extraRead_.reset();
                }

                // Every monitored pipe is closed but the child may still be running: keep
                // enforcing the idle timeout (from the last activity) instead of waiting forever.
                void awaitExit()
                {
                    for (;;)
                    {
                        if (WaitForSingleObject(childProcess_.get(), 0) == WAIT_OBJECT_0) break;
                        if (cancel::isCanceled())
                        {
                            markCanceled(r_);
                            terminateChild();
                            break;
                        }
                        long long pollMs = kExitPollMs;
                        if (anyLimit(opts_))
                        {
                            long long remaining = 0;
                            if (limitExpired(opts_, r_, start_, lastActivity_, Clock::now(), remaining))
                            {
                                terminateChild();
                                break;
                            }
                            pollMs = std::min<long long>(remaining, kExitPollMs);
                        }
                        if (WaitForSingleObject(childProcess_.get(), static_cast<DWORD>(pollMs)) != WAIT_TIMEOUT) break;
                    }
                }

                void collectStatus()
                {
                    WaitForSingleObject(childProcess_.get(), INFINITE);
                    DWORD exitCode = 1;
                    if (GetExitCodeProcess(childProcess_.get(), &exitCode) && !r_.timedOut && !r_.canceled)
                    {
                        r_.exited = true;
                        r_.exitCode = static_cast<int>(exitCode);
                    }
                    childProcess_.reset();
                    job_.reset(); // kills any remaining descendant in the job
                }

                const fs::path &binary_;
                const std::vector<std::string> &args_;
                const Options &opts_;
                const Clock::time_point start_;
                OutputSink sink_;
                Result r_;
                std::vector<char> buf_;

                // Every handle of the parent closes on return (and on an exception)
                UniqueHandle outRead_, outWrite_;
                UniqueHandle extraRead_, extraWrite_;
                UniqueHandle nul_;
                UniqueHandle job_;
                UniqueHandle childProcess_;

                bool inJob_ = false;
                bool childExited_ = false;
                Clock::time_point exitTime_;
                Clock::time_point lastActivity_;
            };

            Result runWindows(const fs::path &binary, const std::vector<std::string> &args, const Options &opts)
            {
                return WindowsRun(binary, args, opts).run();
            }

#endif

        } // namespace

        std::string Result::describe() const
        {
            std::ostringstream oss;
            if (!started)
            {
                oss << "failed to start: " << (errorMessage.empty() ? "unknown error" : errorMessage);
            }
            else if (timedOut || canceled)
            {
                oss << errorMessage;
            }
            else if (termSignal != 0)
            {
                oss << "killed by signal " << termSignal;
#ifndef _WIN32
                if (const char *name = ::strsignal(termSignal)) oss << " (" << name << ")";
#endif
            }
            else if (exited)
            {
                oss << "exited with code " << exitCode;
#ifdef _WIN32
                const unsigned int u = static_cast<unsigned int>(exitCode);
                if (u >= 0xC0000000u)
                    oss << " (0x" << std::hex << std::uppercase << u << ")";
#endif
            }
            else
            {
                oss << "terminated abnormally";
                if (!errorMessage.empty()) oss << ": " << errorMessage;
            }
            if (descendantsTerminated) oss << "; stray descendants terminated after output drain grace";
            return oss.str();
        }

        std::string Result::tail(size_t maxLen) const
        {
            if (outputTail.size() <= maxLen) return outputTail;
            return "..." + outputTail.substr(outputTail.size() - maxLen);
        }

        std::string Result::failureText(size_t tailLength) const
        {
            std::string text = describe();
            if (!outputTail.empty()) text += ": " + tail(tailLength);
            return text;
        }

        Result run(const fs::path &binary, const std::vector<std::string> &args, const Options &options)
        {
#ifdef _WIN32
            Result result = runWindows(binary, args, options);
#else
            Result result = runPosix(binary, args, options);
#endif
            // Surface cleanup even for successful tool calls, whose wrappers may
            // otherwise log only failures or a generic completion message.
            if (result.descendantsTerminated)
                LOGW << io::toUtf8(binary) << " " << result.describe();
            // The child is killed and reaped: the cancellation is the caller's to report
            if (result.canceled)
                cancel::throwIfCanceled();
            return result;
        }

        Result SystemProcessRunner::run(const fs::path &binary, const std::vector<std::string> &args,
                                        const Options &options) const
        {
            return process::run(binary, args, options);
        }

        const IProcessRunner &systemProcessRunner()
        {
            static const SystemProcessRunner runner;
            return runner;
        }

        std::string formatCommandLine(const fs::path &binary, const std::vector<std::string> &args)
        {
            return quotedCommandLine(binary, args);
        }

    } // namespace process
} // namespace ddb
