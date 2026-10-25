/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef PROCESS_SPAWN_H
#define PROCESS_SPAWN_H

// What process::run() and process::Session share and process_runner.h does not export: starting
// a child with exactly the standard streams (and the extra channel) it is given and nothing else,
// reaping and killing it, and collecting what it prints.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "fs.h"
#include "process_runner.h"
#include "unique_handle.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#endif

namespace ddb
{
    namespace process
    {
        namespace detail
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
                OutputSink(size_t capacity, std::function<void(const std::string &)> onLine)
                    : ring_(capacity), onLine_(std::move(onLine)) {}

                explicit OutputSink(const Options &opts) : OutputSink(opts.outputCapacity, opts.onOutputLine) {}

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

                // The most recent bytes fed (at most the capacity)
                std::string tail() const { return ring_.str(); }

              private:
                RingBuffer ring_;
                std::function<void(const std::string &)> onLine_;
                std::string partial_;
                uint64_t total_ = 0;
            };

            // The binary and its arguments as one quoted command line
            std::string quotedCommandLine(const fs::path &binary, const std::vector<std::string> &args);

#ifndef _WIN32

            // pipe with FD_CLOEXEC on both ends (atomic where pipe2 exists); ends[0] is the
            // read end, ends[1] the write end.
            bool makePipe(UniqueFd (&ends)[2]);

            void killGroup(pid_t pid);

            // waitpid with EINTR retry. Returns false on unexpected failure.
            bool waitChild(pid_t pid, int &status);

            // The parent's environment with the overrides applied (the parent's own is never
            // modified).
            std::vector<std::string> childEnvironment(const std::vector<std::pair<std::string, std::string>> &overrides);

            // "<what>: <strerror(errno)>"
            std::string errnoMessage(const char *what);

            // What the child gets: the three standard streams (the same descriptor may serve
            // stdout and stderr), an optional extra channel at kExtraChannelFd, and nothing else.
            struct SpawnRequest
            {
                const std::string &path;
                std::vector<std::string> &argv; // argv[0] included
                std::vector<std::string> &env;
                int stdinFd;
                int stdoutFd;
                int stderrFd;
                int extraWriteFd; // -1: none
                // Linux: SIGKILL the child when the spawning thread exits (one-shot runs, which
                // are supervised by that thread). Long-lived children must not use it: hosts
                // spawn from pool threads that come and go.
                bool dieWithThread;
            };

            // fork() + execve() of the request, then the exec status: the pid, or -1 with `error`
            // saying what failed (the child, if forked, is reaped). The caller's copies of the
            // child's descriptors are left open for the caller to close. Everything the child
            // touches is prepared before fork(), so this is safe from a multi-threaded host.
            pid_t spawnChild(const SpawnRequest &req, std::string &error);

#else

            std::string winErrorMessage(DWORD err);

            // What the child gets: the three standard handles and exactly the inheritable handles
            // listed (the standard ones included), nothing else.
            struct WinSpawnRequest
            {
                const fs::path &binary;
                const std::string &commandLine; // quoted as the child must parse it
                const std::vector<std::pair<std::string, std::string>> &envOverrides;
                HANDLE stdIn;
                HANDLE stdOut;
                HANDLE stdErr;
                std::vector<HANDLE> inherit;
            };

            // A started child: the process and the kill-on-close job it runs in.
            struct WinChild
            {
                UniqueHandle process;
                UniqueHandle job;
                bool inJob = false;

                void terminate()
                {
                    if (inJob)
                        TerminateJobObject(job.get(), 1);
                    else
                        TerminateProcess(process.get(), 1);
                }
            };

            // Creates the child suspended, puts it in a kill-on-close job and lets it run. False:
            // `error` says what failed. The caller's copies of the child's handles stay open for
            // the caller to close.
            bool spawnChild(const WinSpawnRequest &req, WinChild &child, std::string &error);

#endif

        } // namespace detail
    } // namespace process
} // namespace ddb

#endif // PROCESS_SPAWN_H
