/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "process_runner.h"

#include <algorithm>
#include <cerrno>
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
#include "process_spawn.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ddb
{
    namespace process
    {

        // The spawn plumbing run() shares with Session
        using namespace detail;

        namespace
        {

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

#ifndef _WIN32

            // ---------------------------------------------------------------- POSIX

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
                    envStrings_ = childEnvironment(opts.envOverrides);
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
                    if (!makePipe(outPipe_)) return setupFailure("pipe() for output");
                    if (opts_.extraChannel && !makePipe(extraPipe_))
                        return setupFailure("pipe() for extra channel");
                    devNull_.reset(::open("/dev/null", O_RDONLY | O_CLOEXEC));
                    if (!devNull_) return setupFailure("open(/dev/null)");

                    // stdout and stderr share the output pipe
                    const SpawnRequest request{path_,
                                               argStrings_,
                                               envStrings_,
                                               devNull_.get(),
                                               outPipe_[1].get(),
                                               outPipe_[1].get(),
                                               extraPipe_[1].get(),
                                               true};
                    std::string error;
                    pid_ = spawnChild(request, error);
                    outPipe_[1].reset();
                    extraPipe_[1].reset();
                    devNull_.reset();
                    if (pid_ < 0)
                    {
                        r_.errorMessage = error;
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
                // The pipes usually reach EOF a moment before the exiting child can be
                // reaped, so the wait wakes on the exit itself (ExitWatch), not on a tick.
                void awaitExit()
                {
                    ExitWatch exit;
                    exit.watch(pid_);
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
                        pollfd pfd{exit.fd(), POLLIN, 0};
                        ::poll(&pfd, pfd.fd >= 0 ? 1 : 0, static_cast<int>(exit.slice(pollMs)));
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
                    return startChild();
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

                // Starts the child; the parent lets go of the child's ends of the pipes.
                bool startChild()
                {
                    std::vector<HANDLE> inherit = {nul_.get(), outWrite_.get()};
                    if (extraWrite_) inherit.push_back(extraWrite_.get());

                    // The child inherits the handle with the same value: pass it as size_t
                    // (QgisUntwine convention for --progress_fd).
                    const std::string extraId =
                        extraWrite_ ? std::to_string(reinterpret_cast<uintptr_t>(extraWrite_.get()))
                                    : std::string();
                    const std::string commandLine = quotedCommandLine(binary_, expandArgs(args_, opts_, extraId));

                    std::string error;
                    const bool ok = spawnChild({binary_, commandLine, opts_.envOverrides, nul_.get(),
                                                outWrite_.get(), outWrite_.get(), std::move(inherit)},
                                               child_, error);

                    // The parent never uses the child's ends.
                    outWrite_.reset();
                    extraWrite_.reset();
                    nul_.reset();

                    if (!ok)
                    {
                        r_.errorMessage = error;
                        return false;
                    }
                    r_.started = true;
                    return true;
                }

                void terminateChild() { child_.terminate(); }

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
                        if (!childExited_ && WaitForSingleObject(child_.process.get(), 0) == WAIT_OBJECT_0)
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
                            r_.descendantsTerminated = child_.inJob;
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
                        if (WaitForSingleObject(child_.process.get(), 0) == WAIT_OBJECT_0) break;
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
                        if (WaitForSingleObject(child_.process.get(), static_cast<DWORD>(pollMs)) != WAIT_TIMEOUT) break;
                    }
                }

                void collectStatus()
                {
                    WaitForSingleObject(child_.process.get(), INFINITE);
                    DWORD exitCode = 1;
                    if (GetExitCodeProcess(child_.process.get(), &exitCode) && !r_.timedOut && !r_.canceled)
                    {
                        r_.exited = true;
                        r_.exitCode = static_cast<int>(exitCode);
                    }
                    child_.process.reset();
                    child_.job.reset(); // kills any remaining descendant in the job
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
                WinChild child_;

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
