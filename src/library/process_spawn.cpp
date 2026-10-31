/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "process_spawn.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "logger.h"
#include "mio.h"

#ifdef _WIN32
#include <cwchar>
#include <windows.h>
#else
#include <fcntl.h>
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

#ifndef _WIN32

            // Everything the child needs, prepared before fork().
            struct ChildPlan
            {
                const char *path = nullptr;
                char *const *argv = nullptr;
                char *const *envp = nullptr;
                int stdinFd = -1;
                int stdoutFd = -1;
                int stderrFd = -1;
                int extraWrite = -1; // -1 when no extra channel
                int statusWrite = -1;
                long maxFd = 1024;  // upper bound for the close() fallback loop
                pid_t parentPid = 0;
                bool dieWithThread = false; // PR_SET_PDEATHSIG (Linux)
                std::uint64_t maxMemoryBytes = 0; // RLIMIT_AS of the child; 0: unchanged
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
                if (p.dieWithThread)
                {
                    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
                    if (::getppid() != p.parentPid) ::_exit(127);
                }
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
                auto moveUp = [&fail](int fd) {
                    const int moved = ::fcntl(fd, F_DUPFD, kLow);
                    if (moved < 0) fail(errno);
                    return moved;
                };
                const int stdinSrc = moveUp(p.stdinFd);
                const int stdoutSrc = moveUp(p.stdoutFd);
                const int stderrSrc = p.stderrFd == p.stdoutFd ? stdoutSrc : moveUp(p.stderrFd);
                int extraW = -1;
                if (p.extraWrite >= 0) extraW = moveUp(p.extraWrite);
                const int statusTarget = p.extraWrite >= 0 ? kExtraChannelFd + 1 : kExtraChannelFd;
                const int statusW = moveUp(p.statusWrite);

                // dup2 creates the targets without FD_CLOEXEC (inherited by exec).
                if (::dup2(stdinSrc, STDIN_FILENO) < 0) fail(errno);
                if (::dup2(stdoutSrc, STDOUT_FILENO) < 0) fail(errno);
                if (::dup2(stderrSrc, STDERR_FILENO) < 0) fail(errno);
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

                // Last step before the new program: its address space is what the limit bounds.
                // The limit cannot exceed the hard limit this process was given.
                if (p.maxMemoryBytes > 0)
                {
                    struct rlimit rl;
                    if (::getrlimit(RLIMIT_AS, &rl) != 0) fail(errno);
                    const bool tighter = rl.rlim_max == RLIM_INFINITY ||
                                         static_cast<std::uint64_t>(rl.rlim_max) > p.maxMemoryBytes;
                    if (tighter) rl.rlim_max = static_cast<rlim_t>(p.maxMemoryBytes);
                    rl.rlim_cur = rl.rlim_max;
                    if (::setrlimit(RLIMIT_AS, &rl) != 0) fail(errno);
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

#else

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

            // Kill-on-close job: the child (and its descendants) die with us. A non-zero
            // maxMemoryBytes also caps the memory each process of the job may commit. Empty when
            // the job cannot be made.
            UniqueHandle makeChildJob(std::uint64_t maxMemoryBytes)
            {
                UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
                if (job)
                {
                    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
                    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                    if (maxMemoryBytes > 0)
                    {
                        jeli.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
                        jeli.ProcessMemoryLimit = static_cast<SIZE_T>(
                            std::min<std::uint64_t>(maxMemoryBytes, (std::numeric_limits<SIZE_T>::max)()));
                    }
                    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &jeli,
                                                 sizeof(jeli)))
                        job.reset();
                }
                return job;
            }

#endif

        } // namespace

        namespace detail
        {

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

            void killGroup(pid_t pid)
            {
                ::kill(-pid, SIGKILL);
                ::kill(pid, SIGKILL); // in case setpgid did not take effect
            }

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

            void ExitWatch::watch(pid_t pid)
            {
                fallbackMs_ = 1;
#if defined(__linux__) && defined(SYS_pidfd_open)
                // The descriptor is close-on-exec. ENOSYS (before Linux 5.3) or a full
                // descriptor table leaves the bounded fallback poll in charge.
                pidfd_.reset(static_cast<int>(::syscall(SYS_pidfd_open, pid, 0)));
#else
                (void)pid;
                pidfd_.reset();
#endif
            }

            long long ExitWatch::slice(long long ms)
            {
                if (pidfd_) return ms;
                const long long slice = std::min(ms, fallbackMs_);
                fallbackMs_ = std::min(fallbackMs_ * 2, kExitPollMs);
                return slice;
            }

            std::vector<std::string> childEnvironment(const std::vector<std::pair<std::string, std::string>> &overrides)
            {
                std::vector<std::string> envStrings;
                for (char **e = environ; e && *e; ++e)
                {
                    const std::string entry(*e);
                    const std::string name = entry.substr(0, entry.find('='));
                    bool overridden = false;
                    for (const auto &ov : overrides)
                    {
                        if (ov.first == name)
                        {
                            overridden = true;
                            break;
                        }
                    }
                    if (!overridden) envStrings.push_back(entry);
                }
                for (const auto &ov : overrides)
                    envStrings.push_back(ov.first + "=" + ov.second);
                return envStrings;
            }

            std::string errnoMessage(const char *what)
            {
                return std::string(what) + ": " + std::strerror(errno);
            }

            pid_t spawnChild(const SpawnRequest &req, std::string &error)
            {
                std::vector<char *> argv = cStringPointers(req.argv);
                std::vector<char *> envp = cStringPointers(req.env);

                UniqueFd statusPipe[2];
                if (!makePipe(statusPipe))
                {
                    error = errnoMessage("pipe() for exec status");
                    return -1;
                }

                ChildPlan plan;
                plan.path = req.path.c_str();
                plan.argv = argv.data();
                plan.envp = envp.data();
                plan.stdinFd = req.stdinFd;
                plan.stdoutFd = req.stdoutFd;
                plan.stderrFd = req.stderrFd;
                plan.extraWrite = req.extraWriteFd;
                plan.statusWrite = statusPipe[1].get();
                plan.maxFd = computeMaxFd();
                plan.parentPid = ::getpid();
                plan.dieWithThread = req.dieWithThread;
                plan.maxMemoryBytes = req.maxMemoryBytes;

                const pid_t pid = ::fork();
                if (pid < 0)
                {
                    error = errnoMessage("fork()");
                    return -1;
                }
                if (pid == 0) childExec(plan);

                ::setpgid(pid, pid); // mirror the child's call (avoids a kill(-pid) race)
                statusPipe[1].reset();

                // Exec status: EOF means execve succeeded (CLOEXEC), an int is errno.
                int execErr = 0;
                ssize_t sn;
                do
                {
                    sn = ::read(statusPipe[0].get(), &execErr, sizeof(execErr));
                } while (sn < 0 && errno == EINTR);

                if (sn == static_cast<ssize_t>(sizeof(execErr)))
                {
                    int status = 0;
                    waitChild(pid, status);
                    error = std::strerror(execErr);
                    return -1;
                }
                return pid;
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

            bool spawnChild(const WinSpawnRequest &req, WinChild &child, std::string &error)
            {
                SIZE_T attrSize = 0;
                InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
                std::vector<unsigned char> attrBuf(attrSize);
                auto attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
                if (!InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize))
                {
                    error = "InitializeProcThreadAttributeList: " + winErrorMessage(GetLastError());
                    return false;
                }
                if (!UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                               const_cast<HANDLE *>(req.inherit.data()),
                                               req.inherit.size() * sizeof(HANDLE), nullptr, nullptr))
                {
                    const DWORD err = GetLastError();
                    DeleteProcThreadAttributeList(attrList);
                    error = "UpdateProcThreadAttribute: " + winErrorMessage(err);
                    return false;
                }

                STARTUPINFOEXW siex{};
                siex.StartupInfo.cb = sizeof(siex);
                siex.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
                siex.StartupInfo.hStdInput = req.stdIn;
                siex.StartupInfo.hStdOutput = req.stdOut;
                siex.StartupInfo.hStdError = req.stdErr;
                siex.lpAttributeList = attrList;

                std::wstring wideCmd = io::toWide(req.commandLine);
                wideCmd.push_back(L'\0'); // CreateProcessW may modify the buffer
                const std::wstring wideBin = req.binary.wstring();

                std::vector<wchar_t> envBlock;
                DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED;
                if (!req.envOverrides.empty())
                {
                    envBlock = buildEnvBlock(req.envOverrides);
                    flags |= CREATE_UNICODE_ENVIRONMENT;
                }

                child.job = makeChildJob(req.maxMemoryBytes);

                PROCESS_INFORMATION pi{};
                const BOOL ok = CreateProcessW(wideBin.c_str(), wideCmd.data(), nullptr, nullptr, TRUE, flags,
                                               envBlock.empty() ? nullptr : envBlock.data(), nullptr,
                                               &siex.StartupInfo, &pi);
                const DWORD createErr = ok ? 0 : GetLastError();
                DeleteProcThreadAttributeList(attrList);
                if (!ok)
                {
                    error = winErrorMessage(createErr);
                    return false;
                }

                child.process.reset(pi.hProcess);
                UniqueHandle childThread(pi.hThread);
                child.inJob = child.job && AssignProcessToJobObject(child.job.get(), child.process.get());
                if (!child.inJob)
                {
                    const DWORD assignErr = GetLastError();
                    // Without the job the memory limit would silently not apply: do not run the
                    // child unbounded when the caller asked for a bound.
                    if (req.maxMemoryBytes > 0)
                    {
                        TerminateProcess(child.process.get(), 1);
                        error = "could not apply the memory limit to the child process (job object, error " +
                                std::to_string(assignErr) + ")";
                        return false;
                    }
                    LOGD << "Could not assign child process to a job object (error " << assignErr
                         << "); orphan protection is unavailable";
                }
                ResumeThread(childThread.get());
                return true;
            }

#endif

        } // namespace detail

    } // namespace process
} // namespace ddb
