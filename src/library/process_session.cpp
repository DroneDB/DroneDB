/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// process::Session (process_runner.h): a long-lived child spoken to through its stdin and stdout
// while its stderr is drained.

#include "process_runner.h"

#include <algorithm>
#include <cerrno>
#include <memory>
#include <string>
#include <vector>

#include "unique_handle.h"
#include "mio.h"
#include "process_spawn.h"

#ifdef _WIN32
#include <atomic>
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ddb
{
    namespace process
    {

        // The spawn plumbing Session shares with run()
        using namespace detail;

        namespace
        {

            // Milliseconds until the deadline, rounded up: 0 once it has passed. Capped at an hour
            // (callers loop), so it fits a poll() or Wait timeout whatever the deadline.
            long long msUntil(Clock::time_point deadline)
            {
                const auto left = deadline - Clock::now();
                if (left <= Clock::duration::zero()) return 0;
                if (left > std::chrono::hours(1)) return 3600000;
                return std::chrono::ceil<milliseconds>(left).count();
            }

            // Upper bound of the reads made in one go to empty the child's stderr, so a
            // descendant that never stops writing cannot keep the caller in the loop.
            constexpr int kMaxDrainReads = 256;

            // The bytes read from the child's stdout, cut into lines on demand.
            class LineBuffer
            {
              public:
                enum class Take
                {
                    Line,
                    TooLong,
                    Incomplete
                };

                void append(const char *data, size_t n) { data_.append(data, n); }

                // Moves the next line (without '\n' and the '\r' before it) to `line`. TooLong: the
                // next line is longer than maxBytes (nothing is consumed).
                Take take(size_t maxBytes, std::string &line)
                {
                    const size_t nl = data_.find('\n', std::max(scanned_, head_));
                    if (nl == std::string::npos)
                    {
                        scanned_ = data_.size();
                        return data_.size() - head_ > maxBytes ? Take::TooLong : Take::Incomplete;
                    }
                    if (nl - head_ > maxBytes) return Take::TooLong;
                    line.assign(data_, head_, nl - head_);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    consume(nl + 1);
                    return Take::Line;
                }

                // The bytes after the last line, unterminated.
                std::string takeRest()
                {
                    std::string rest = data_.substr(head_);
                    consume(data_.size());
                    return rest;
                }

              private:
                void consume(size_t upTo)
                {
                    head_ = upTo;
                    scanned_ = head_;
                    if (head_ == data_.size())
                    {
                        data_.clear();
                        head_ = scanned_ = 0;
                    }
                    else if (head_ >= kReadChunk && head_ * 2 >= data_.size())
                    {
                        data_.erase(0, head_);
                        head_ = scanned_ = 0;
                    }
                }

                std::string data_;
                size_t head_ = 0;    // start of the first unread line
                size_t scanned_ = 0; // no '\n' in [head_, scanned_)
            };

        } // namespace

        // What both platforms share of a session: the stdout line framing, the stderr ring buffer
        // and the exit status. The platform part moves the bytes and owns the child.
        class SessionImpl
        {
          public:
            explicit SessionImpl(const SessionOptions &options)
                : stderrSink_(options.stderrCapacity, options.onStderrLine) {}
            virtual ~SessionImpl() = default;

            // Starts the child; false: `error` says why.
            virtual bool launch(const fs::path &binary, const std::vector<std::string> &args,
                                const SessionOptions &options, std::string &error) = 0;
            virtual Session::WriteStatus write(const char *data, size_t size, Clock::time_point deadline) = 0;
            virtual void closeStdin() = 0;
            virtual bool wait(Clock::time_point deadline) = 0;
            virtual void kill() = 0;
            virtual unsigned long pid() const = 0;

            Session::ReadResult readLine(Clock::time_point deadline, size_t maxBytes)
            {
                Session::ReadResult result;
                for (;;)
                {
                    switch (lines_.take(maxBytes, result.line))
                    {
                    case LineBuffer::Take::Line:
                        result.status = Session::ReadStatus::Line;
                        return result;
                    case LineBuffer::Take::TooLong:
                        result.line.clear();
                        result.status = Session::ReadStatus::TooLong;
                        return result;
                    case LineBuffer::Take::Incomplete:
                        break;
                    }
                    if (stdoutEof_)
                    {
                        result.line = lines_.takeRest();
                        result.status = Session::ReadStatus::Eof;
                        return result;
                    }
                    if (!awaitStdout(deadline))
                    {
                        result.line.clear();
                        result.status = Session::ReadStatus::Timeout;
                        return result;
                    }
                }
            }

            bool finished() const { return finished_; }
            Session::ExitStatus exitStatus() const { return exit_; }

            std::string describeExit() const
            {
                Result r;
                r.started = true;
                r.exited = exit_.exited;
                r.exitCode = exit_.exitCode;
                r.termSignal = exit_.termSignal;
                return finished_ ? r.describe() : std::string("still running");
            }

            std::string stderrTail() const { return stderrSink_.tail(); }

          protected:
            // Waits until more of stdout was buffered (appendStdout) or it reached its end
            // (markStdoutEof), draining stderr meanwhile. False: the deadline passed first.
            virtual bool awaitStdout(Clock::time_point deadline) = 0;

            void appendStdout(const char *data, size_t n) { lines_.append(data, n); }
            void markStdoutEof() { stdoutEof_ = true; }
            void feedStderr(const char *data, size_t n) { stderrSink_.feed(data, n); }
            void flushStderr() { stderrSink_.flush(); }

            void recordExit(const Session::ExitStatus &status)
            {
                exit_ = status;
                finished_ = true;
            }

          private:
            LineBuffer lines_;
            OutputSink stderrSink_;
            bool stdoutEof_ = false;
            bool finished_ = false;
            Session::ExitStatus exit_;
        };

        namespace
        {

#ifndef _WIN32

            // socketpair with FD_CLOEXEC on both ends; ends[0] is the parent's.
            bool makeSocketPair(UniqueFd (&ends)[2])
            {
                int fds[2];
#ifdef SOCK_CLOEXEC
                if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) return false;
#else
                if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return false;
                ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
                ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#endif
                ends[0].reset(fds[0]);
                ends[1].reset(fds[1]);
                return true;
            }

            bool setNonBlocking(int fd)
            {
                const int flags = ::fcntl(fd, F_GETFL, 0);
                return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
            }

            // Writing to a socket whose peer is gone must return EPIPE, not raise SIGPIPE in the
            // host (which may ignore it or not; its signal handling is none of our business).
#ifdef MSG_NOSIGNAL
            constexpr int kSendFlags = MSG_NOSIGNAL;
            void suppressSigpipe(int) {}
#else
            constexpr int kSendFlags = 0;
            void suppressSigpipe([[maybe_unused]] int fd)
            {
#ifdef SO_NOSIGPIPE
                const int on = 1;
                ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
            }
#endif

            // One read from a non-blocking descriptor: the byte count; -1 when there is nothing
            // right now; 0 at the end (or on an unrecoverable error), when the descriptor is closed.
            ssize_t readSome(UniqueFd &fd, char *buf, size_t size)
            {
                const ssize_t n = ::read(fd.get(), buf, size);
                if (n > 0) return n;
                if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) return -1;
                fd.reset();
                return 0;
            }

            // The POSIX session: stdin is a socketpair end (send() with MSG_NOSIGNAL never
            // raises SIGPIPE), stdout and stderr are pipes, all non-blocking on the parent's
            // side and polled together so neither can fill up while the other is waited for.
            class PosixSession final : public SessionImpl
            {
              public:
                using SessionImpl::SessionImpl;
                ~PosixSession() override { kill(); }

                bool launch(const fs::path &binary, const std::vector<std::string> &args,
                            const SessionOptions &options, std::string &error) override
                {
                    UniqueFd stdinPair[2], outPipe[2], errPipe[2];
                    if (!makeSocketPair(stdinPair))
                    {
                        error = errnoMessage("socketpair() for stdin");
                        return false;
                    }
                    if (!makePipe(outPipe))
                    {
                        error = errnoMessage("pipe() for output");
                        return false;
                    }
                    if (!makePipe(errPipe))
                    {
                        error = errnoMessage("pipe() for error output");
                        return false;
                    }
                    if (!setNonBlocking(stdinPair[0].get()) || !setNonBlocking(outPipe[0].get()) ||
                        !setNonBlocking(errPipe[0].get()))
                    {
                        error = errnoMessage("fcntl(O_NONBLOCK)");
                        return false;
                    }
                    suppressSigpipe(stdinPair[0].get());

                    const std::string path = io::toUtf8(binary);
                    std::vector<std::string> argv;
                    argv.reserve(args.size() + 1);
                    argv.push_back(path);
                    argv.insert(argv.end(), args.begin(), args.end());
                    std::vector<std::string> env = childEnvironment(options.envOverrides);

                    const SpawnRequest request{path,
                                               argv,
                                               env,
                                               stdinPair[1].get(),
                                               outPipe[1].get(),
                                               errPipe[1].get(),
                                               -1,
                                               false};
                    pid_ = spawnChild(request, error);
                    if (pid_ < 0) return false;

                    stdin_ = std::move(stdinPair[0]);
                    stdout_ = std::move(outPipe[0]);
                    stderr_ = std::move(errPipe[0]);
                    // Lets wait() sleep until the exit instead of polling for it
                    exit_.watch(pid_);
                    buf_.resize(kReadChunk);
                    return true;
                }

                Session::WriteStatus write(const char *data, size_t size, Clock::time_point deadline) override
                {
                    size_t sent = 0;
                    while (sent < size)
                    {
                        if (!stdin_) return Session::WriteStatus::Closed;
                        const ssize_t n = ::send(stdin_.get(), data + sent, size - sent, kSendFlags);
                        if (n > 0)
                        {
                            sent += static_cast<size_t>(n);
                            continue;
                        }
                        if (n < 0 && errno == EINTR) continue;
                        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                        {
                            // The child is not reading: wait for room, draining its stderr.
                            pollfd pfds[2];
                            nfds_t count = 0;
                            pfds[count++] = {stdin_.get(), POLLOUT, 0};
                            if (stderr_) pfds[count++] = {stderr_.get(), POLLIN, 0};
                            const int pr = ::poll(pfds, count, static_cast<int>(msUntil(deadline)));
                            if (pr < 0 && errno != EINTR) return Session::WriteStatus::Closed;
                            if (pr > 0 && count == 2 && pfds[1].revents != 0) pumpStderr();
                            // Room (or an error send() reports) goes on; anything else, a busy
                            // stderr included, ends at the deadline
                            if (pr > 0 && pfds[0].revents != 0) continue;
                            if (Clock::now() >= deadline) return Session::WriteStatus::Timeout;
                            continue;
                        }
                        return Session::WriteStatus::Closed; // EPIPE, ECONNRESET...
                    }
                    return Session::WriteStatus::Ok;
                }

                void closeStdin() override { stdin_.reset(); }

                bool wait(Clock::time_point deadline) override
                {
                    for (;;)
                    {
                        if (tryReap()) return true;
                        pollfd pfds[2];
                        nfds_t count = 0;
                        if (exit_.fd() >= 0) pfds[count++] = {exit_.fd(), POLLIN, 0};
                        const nfds_t stderrSlot = count;
                        if (stderr_) pfds[count++] = {stderr_.get(), POLLIN, 0};
                        const int pr = ::poll(pfds, count, static_cast<int>(exit_.slice(msUntil(deadline))));
                        if (pr > 0 && stderrSlot < count && pfds[stderrSlot].revents != 0) pumpStderr();
                        if (Clock::now() >= deadline) return tryReap();
                    }
                }

                void kill() override
                {
                    if (finished() || pid_ <= 0) return;
                    killGroup(pid_);
                    int status = 0;
                    const bool known = waitChild(pid_, status);
                    recordWaitStatus(known, status);
                }

                unsigned long pid() const override { return pid_ > 0 ? static_cast<unsigned long>(pid_) : 0; }

              protected:
                bool awaitStdout(Clock::time_point deadline) override
                {
                    for (;;)
                    {
                        if (!stdout_)
                        {
                            markStdoutEof();
                            return true;
                        }
                        pollfd pfds[2];
                        nfds_t count = 0;
                        pfds[count++] = {stdout_.get(), POLLIN, 0};
                        if (stderr_) pfds[count++] = {stderr_.get(), POLLIN, 0};
                        const int pr = ::poll(pfds, count, static_cast<int>(msUntil(deadline)));
                        if (pr < 0 && errno != EINTR)
                        {
                            markStdoutEof(); // the descriptor is unusable: report the end
                            return true;
                        }
                        bool progress = false;
                        if (pr > 0)
                        {
                            if (count == 2 && pfds[1].revents != 0) pumpStderr();
                            if (pfds[0].revents != 0) progress = pumpStdout();
                        }
                        if (progress) return true;
                        if (Clock::now() >= deadline) return false;
                    }
                }

              private:
                // True when stdout produced bytes or ended.
                bool pumpStdout()
                {
                    const ssize_t n = readSome(stdout_, buf_.data(), buf_.size());
                    if (n > 0) appendStdout(buf_.data(), static_cast<size_t>(n));
                    else if (n == 0) markStdoutEof();
                    return n >= 0;
                }

                // One read of the child's stderr; true when it produced bytes.
                bool pumpStderr()
                {
                    const ssize_t n = readSome(stderr_, buf_.data(), buf_.size());
                    if (n > 0) feedStderr(buf_.data(), static_cast<size_t>(n));
                    else if (n == 0) flushStderr();
                    return n > 0;
                }

                // What is left in the pipe once the child is gone.
                void drainStderr()
                {
                    for (int i = 0; i < kMaxDrainReads && stderr_ && pumpStderr(); ++i)
                    {
                    }
                }

                bool tryReap()
                {
                    if (finished()) return true;
                    int status = 0;
                    pid_t w;
                    do
                    {
                        w = ::waitpid(pid_, &status, WNOHANG);
                    } while (w < 0 && errno == EINTR);
                    if (w == 0) return false;
                    recordWaitStatus(w == pid_, status);
                    return true;
                }

                // `known` false: someone else reaped the child (a host that ignores SIGCHLD); the
                // status is lost.
                void recordWaitStatus(bool known, int status)
                {
                    Session::ExitStatus ended;
                    if (known && WIFEXITED(status))
                    {
                        ended.exited = true;
                        ended.exitCode = WEXITSTATUS(status);
                    }
                    else if (known && WIFSIGNALED(status))
                    {
                        ended.termSignal = WTERMSIG(status);
                    }
                    recordExit(ended);
                    drainStderr();
                }

                UniqueFd stdin_, stdout_, stderr_;
                ExitWatch exit_;
                std::vector<char> buf_;
                pid_t pid_ = -1;
            };

            std::unique_ptr<SessionImpl> makeSessionImpl(const SessionOptions &options)
            {
                return std::make_unique<PosixSession>(options);
            }

#else

            // A pipe whose parent end supports overlapped I/O: anonymous pipes do not, and
            // polling them with PeekNamedPipe would add a sleep to every read. The parent end is
            // the named pipe's server side (not inheritable), the child end a plain inheritable
            // handle opened on it.
            bool makeOverlappedPipe(bool parentReads, UniqueHandle &parentEnd, UniqueHandle &childEnd,
                                    std::string &error)
            {
                static std::atomic<unsigned> counter{0};
                DWORD err = ERROR_SUCCESS;
                for (int attempt = 0; attempt < 8; ++attempt)
                {
                    const std::wstring name = L"\\\\.\\pipe\\ddb-session-" + std::to_wstring(GetCurrentProcessId()) +
                                              L"-" + std::to_wstring(counter.fetch_add(1)) + L"-" +
                                              std::to_wstring(GetTickCount64());
                    const DWORD openMode = (parentReads ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND) |
                                           FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE;
                    parentEnd.reset(CreateNamedPipeW(name.c_str(), openMode,
                                                     PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                                         PIPE_REJECT_REMOTE_CLIENTS,
                                                     1, static_cast<DWORD>(kReadChunk),
                                                     static_cast<DWORD>(kReadChunk), 0, nullptr));
                    if (!parentEnd)
                    {
                        err = GetLastError();
                        if (err == ERROR_ACCESS_DENIED || err == ERROR_PIPE_BUSY) continue; // name taken
                        break;
                    }
                    SECURITY_ATTRIBUTES saInherit{};
                    saInherit.nLength = sizeof(saInherit);
                    saInherit.bInheritHandle = TRUE;
                    // The client's CreateFile connects the pipe: no ConnectNamedPipe is needed.
                    childEnd.reset(CreateFileW(name.c_str(), parentReads ? GENERIC_WRITE : GENERIC_READ, 0,
                                               &saInherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
                    if (!childEnd)
                    {
                        err = GetLastError();
                        parentEnd.reset();
                        break;
                    }
                    return true;
                }
                error = "CreateNamedPipe: " + winErrorMessage(err);
                return false;
            }

            // The parent end of a pipe the child writes to, with at most one overlapped read in
            // flight. A read that is still pending when the caller's deadline passes stays
            // pending: the next call picks it up, so no byte is lost and nothing is polled.
            class OverlappedReader
            {
              public:
                OverlappedReader() : buf_(kReadChunk) {}
                ~OverlappedReader() { cancel(); }
                OverlappedReader(const OverlappedReader &) = delete;
                OverlappedReader &operator=(const OverlappedReader &) = delete;

                bool attach(UniqueHandle pipe)
                {
                    pipe_ = std::move(pipe);
                    event_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr)); // manual reset
                    return event_.valid();
                }

                bool open() const { return pipe_.valid(); }

                // Signaled when the pending read has completed (data, end or error).
                HANDLE event() const { return event_.get(); }

                // Starts a read when none is pending. The pipe is closed if the read cannot start
                // (ERROR_BROKEN_PIPE: the writer is gone and everything was read).
                void arm()
                {
                    if (!pipe_ || pending_) return;
                    ov_ = OVERLAPPED{};
                    ov_.hEvent = event_.get();
                    if (ReadFile(pipe_.get(), buf_.data(), static_cast<DWORD>(buf_.size()), nullptr, &ov_) ||
                        GetLastError() == ERROR_IO_PENDING)
                        pending_ = true; // completed or not, the event reports it
                    else
                        pipe_.reset();
                }

                // Takes the result of a completed read: appends its bytes to `out`; the pipe is
                // closed at the end. False while the read is still pending.
                bool harvest(std::string &out)
                {
                    if (!pending_) return false;
                    DWORD n = 0;
                    if (!GetOverlappedResult(pipe_.get(), &ov_, &n, FALSE))
                    {
                        if (GetLastError() == ERROR_IO_INCOMPLETE) return false;
                        pending_ = false;
                        pipe_.reset(); // ERROR_BROKEN_PIPE and the like: the end
                        return true;
                    }
                    pending_ = false;
                    if (n == 0)
                        pipe_.reset();
                    else
                        out.append(buf_.data(), n);
                    return true;
                }

              private:
                // The kernel writes into buf_ and ov_ until the read is done.
                void cancel()
                {
                    if (!pending_) return;
                    CancelIoEx(pipe_.get(), &ov_);
                    DWORD n = 0;
                    GetOverlappedResult(pipe_.get(), &ov_, &n, TRUE);
                    pending_ = false;
                }

                std::vector<char> buf_;
                OVERLAPPED ov_{};
                bool pending_ = false;
                UniqueHandle pipe_;
                UniqueHandle event_;
            };

            // The Windows session: the parent ends are overlapped named pipes, so every wait is
            // a WaitForMultipleObjects on events (stdout, stderr, the process) with the caller's
            // deadline: the latency of a read is the pipe's. The child runs in a kill-on-close
            // job the session holds.
            class WindowsSession final : public SessionImpl
            {
              public:
                using SessionImpl::SessionImpl;
                ~WindowsSession() override { kill(); }

                bool launch(const fs::path &binary, const std::vector<std::string> &args,
                            const SessionOptions &options, std::string &error) override
                {
                    UniqueHandle outParent, errParent, childIn, childOut, childErr;
                    if (!makeOverlappedPipe(false, stdin_, childIn, error) ||
                        !makeOverlappedPipe(true, outParent, childOut, error) ||
                        !makeOverlappedPipe(true, errParent, childErr, error))
                        return false;
                    writeEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                    if (!writeEvent_ || !stdout_.attach(std::move(outParent)) || !stderr_.attach(std::move(errParent)))
                    {
                        error = "CreateEvent: " + winErrorMessage(GetLastError());
                        return false;
                    }

                    const std::string commandLine = quotedCommandLine(binary, args);
                    // The child's ends close when this returns: the parent never uses them
                    return spawnChild({binary, commandLine, options.envOverrides, childIn.get(), childOut.get(),
                                       childErr.get(), {childIn.get(), childOut.get(), childErr.get()}},
                                      child_, error);
                }

                Session::WriteStatus write(const char *data, size_t size, Clock::time_point deadline) override
                {
                    size_t sent = 0;
                    while (sent < size)
                    {
                        if (!stdin_) return Session::WriteStatus::Closed;
                        drainStderr();
                        OVERLAPPED ov{};
                        ov.hEvent = writeEvent_.get();
                        const DWORD want = static_cast<DWORD>(std::min<size_t>(size - sent, kReadChunk));
                        DWORD written = 0;
                        if (!WriteFile(stdin_.get(), data + sent, want, nullptr, &ov))
                        {
                            if (GetLastError() != ERROR_IO_PENDING) return Session::WriteStatus::Closed;
                            if (!awaitWrite(ov, deadline)) // the child does not read: give up on it
                            {
                                CancelIoEx(stdin_.get(), &ov);
                                if (GetOverlappedResult(stdin_.get(), &ov, &written, TRUE)) sent += written;
                                return sent == size ? Session::WriteStatus::Ok : Session::WriteStatus::Timeout;
                            }
                        }
                        if (!GetOverlappedResult(stdin_.get(), &ov, &written, FALSE)) return Session::WriteStatus::Closed;
                        sent += written;
                    }
                    return Session::WriteStatus::Ok;
                }

                void closeStdin() override { stdin_.reset(); }

                bool wait(Clock::time_point deadline) override
                {
                    if (finished()) return true;
                    if (!child_.process) return false;
                    for (;;)
                    {
                        stderr_.arm();
                        HANDLE handles[2] = {child_.process.get(), nullptr};
                        DWORD count = 1;
                        if (stderr_.open()) handles[count++] = stderr_.event();
                        const DWORD w = WaitForMultipleObjects(count, handles, FALSE, static_cast<DWORD>(msUntil(deadline)));
                        if (w == WAIT_OBJECT_0)
                        {
                            collectExit(false);
                            return true;
                        }
                        if (count == 2 && w == WAIT_OBJECT_0 + 1)
                            pumpStderr();
                        else if (w != WAIT_TIMEOUT)
                            return false; // WAIT_FAILED
                        // A busy stderr does not hold the caller past its deadline
                        if (Clock::now() >= deadline) return false;
                    }
                }

                void kill() override
                {
                    if (finished() || !child_.process) return;
                    child_.terminate();
                    WaitForSingleObject(child_.process.get(), INFINITE);
                    collectExit(true);
                }

                unsigned long pid() const override
                {
                    return child_.process ? static_cast<unsigned long>(GetProcessId(child_.process.get())) : 0;
                }

              protected:
                bool awaitStdout(Clock::time_point deadline) override
                {
                    for (;;)
                    {
                        stdout_.arm();
                        stderr_.arm();
                        if (!stdout_.open())
                        {
                            markStdoutEof();
                            return true;
                        }
                        HANDLE handles[2] = {stdout_.event(), nullptr};
                        DWORD count = 1;
                        if (stderr_.open()) handles[count++] = stderr_.event();
                        const DWORD w = WaitForMultipleObjects(count, handles, FALSE, static_cast<DWORD>(msUntil(deadline)));
                        if (w == WAIT_OBJECT_0)
                        {
                            std::string data;
                            if (stdout_.harvest(data))
                            {
                                if (!data.empty()) appendStdout(data.data(), data.size());
                                if (!stdout_.open()) markStdoutEof();
                                return true;
                            }
                        }
                        else if (count == 2 && w == WAIT_OBJECT_0 + 1)
                            pumpStderr();
                        else if (w != WAIT_TIMEOUT)
                        {
                            markStdoutEof(); // the wait failed: report the end
                            return true;
                        }
                        // A busy stderr does not hold the caller past its deadline
                        if (Clock::now() >= deadline) return false;
                    }
                }

              private:
                // Waits for the pending write, draining stderr meanwhile. False: the deadline passed.
                bool awaitWrite(OVERLAPPED &ov, Clock::time_point deadline)
                {
                    for (;;)
                    {
                        stderr_.arm();
                        HANDLE handles[2] = {ov.hEvent, nullptr};
                        DWORD count = 1;
                        if (stderr_.open()) handles[count++] = stderr_.event();
                        const DWORD w = WaitForMultipleObjects(count, handles, FALSE, static_cast<DWORD>(msUntil(deadline)));
                        if (w == WAIT_OBJECT_0) return true;
                        if (count == 2 && w == WAIT_OBJECT_0 + 1)
                            pumpStderr();
                        else if (w != WAIT_TIMEOUT)
                            return false; // WAIT_FAILED
                        // A busy stderr does not hold the write past its deadline
                        if (Clock::now() >= deadline) return false;
                    }
                }

                // Takes a completed stderr read, if any. True when there was one.
                bool pumpStderr()
                {
                    std::string data;
                    if (!stderr_.harvest(data)) return false;
                    if (!data.empty()) feedStderr(data.data(), data.size());
                    if (!stderr_.open()) flushStderr();
                    return true;
                }

                // Everything stderr holds right now, without waiting.
                void drainStderr()
                {
                    for (int i = 0; i < kMaxDrainReads; ++i)
                    {
                        stderr_.arm();
                        if (!stderr_.open() || WaitForSingleObject(stderr_.event(), 0) != WAIT_OBJECT_0) return;
                        pumpStderr();
                    }
                }

                // The process has ended: records how, ends the job (any descendant still in it
                // dies, as after run()) and collects what stderr has left.
                void collectExit(bool killed)
                {
                    DWORD code = 1;
                    GetExitCodeProcess(child_.process.get(), &code);
                    Session::ExitStatus ended;
                    if (!killed)
                    {
                        ended.exited = true;
                        ended.exitCode = static_cast<int>(code);
                    }
                    recordExit(ended);
                    child_.job.reset();
                    drainStderr();
                }

                UniqueHandle stdin_;
                UniqueHandle writeEvent_;
                OverlappedReader stdout_, stderr_;
                WinChild child_;
            };

            std::unique_ptr<SessionImpl> makeSessionImpl(const SessionOptions &options)
            {
                return std::make_unique<WindowsSession>(options);
            }

#endif

        } // namespace

        Session::Session() = default;
        Session::~Session() = default;
        Session::Session(Session &&) noexcept = default;
        Session &Session::operator=(Session &&) noexcept = default;

        bool Session::start(const fs::path &binary, const std::vector<std::string> &args,
                            const SessionOptions &options)
        {
            if (impl_)
            {
                startError_ = "the session is already started";
                return false;
            }
            auto impl = makeSessionImpl(options);
            startError_.clear();
            if (!impl->launch(binary, args, options, startError_)) return false;
            impl_ = std::move(impl);
            return true;
        }

        Session::WriteStatus Session::write(const std::string &bytes, Clock::time_point deadline)
        {
            return impl_ ? impl_->write(bytes.data(), bytes.size(), deadline) : WriteStatus::Closed;
        }

        Session::WriteStatus Session::writeLine(const std::string &line, Clock::time_point deadline)
        {
            // One buffer, so a short line goes out in one write
            return write(line + '\n', deadline);
        }

        Session::ReadResult Session::readLine(Clock::time_point deadline, size_t maxBytes)
        {
            return impl_ ? impl_->readLine(deadline, maxBytes) : ReadResult();
        }

        void Session::closeStdin()
        {
            if (impl_) impl_->closeStdin();
        }

        bool Session::wait(Clock::time_point deadline)
        {
            return impl_ && impl_->wait(deadline);
        }

        void Session::kill()
        {
            if (impl_) impl_->kill();
        }

        bool Session::finished() const
        {
            return impl_ && impl_->finished();
        }

        Session::ExitStatus Session::exitStatus() const
        {
            return impl_ ? impl_->exitStatus() : ExitStatus();
        }

        std::string Session::describeExit() const
        {
            return impl_ ? impl_->describeExit() : std::string("not started");
        }

        unsigned long Session::pid() const
        {
            return impl_ ? impl_->pid() : 0;
        }

        std::string Session::stderrTail() const
        {
            return impl_ ? impl_->stderrTail() : std::string();
        }

    } // namespace process
} // namespace ddb
