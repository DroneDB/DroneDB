/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef PROCESS_RUNNER_H
#define PROCESS_RUNNER_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "fs.h"

namespace ddb
{

    // Hardened child-process launcher shared by the external tool runners
    // (Obj2Tiles, Untwine, build-lod).
    //
    // The child gets:
    //   - stdin connected to the null device (/dev/null, NUL);
    //   - stdout and stderr merged into one pipe read by the parent;
    //   - optionally one extra "channel" pipe (write end) the parent reads too
    //     (Untwine's --progress_fd);
    //   - no other inherited descriptor/handle (POSIX: every other fd is closed
    //     in the child; Windows: explicit PROC_THREAD_ATTRIBUTE_HANDLE_LIST);
    //   - the parent's environment, with optional per-child overrides (the
    //     parent's own environment is never modified);
    //   - orphan protection: POSIX children run in their own process group and,
    //     on Linux, receive SIGKILL when the spawning thread dies
    //     (PR_SET_PDEATHSIG); Windows children are placed in a Job Object with
    //     JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE.
    //
    // POSIX spawning uses fork() + execve() rather than posix_spawn(): the child
    // needs setpgid, PR_SET_PDEATHSIG, a reset signal mask/SIGPIPE and
    // close_range, which posix_spawn cannot all express portably. The cost is a
    // page-table copy proportional to the parent's resident size on every tool
    // launch (tens of milliseconds for a multi-GB host such as the Registry) and,
    // under strict overcommit (vm.overcommit_memory=2), fork() may fail with ENOMEM
    // where posix_spawn would not; that failure is reported in Result::errorMessage
    // ("fork(): Cannot allocate memory"). Switch to posix_spawn with
    // posix_spawnattr_setpgroup/SETSIGMASK/SETSIGDEF and
    // posix_spawn_file_actions_addclosefrom_np (glibc 2.34) if that ever shows up
    // in the field; only PR_SET_PDEATHSIG would be lost.
    //
    // The parent reads both pipes until EOF and kills the child (its whole
    // process group / job) when no byte arrives on ANY monitored pipe for the
    // idle timeout. run() blocks the calling thread (the child is forked on it)
    // and never throws for child failures: everything is reported in Result.
    // It checks the cancellation of the calling thread's work (cancellation.h)
    // at least every 500 ms: when it is canceled, the child is killed and
    // reaped, and run() throws CanceledException (ShutdownException).
    namespace process
    {

        // Size of the bounded output ring buffer (tail of stdout+stderr kept in
        // Result::outputTail). Older bytes are discarded.
        constexpr size_t kDefaultOutputCapacity = 64 * 1024;

        // Default idle timeout: the child is killed when it produces no output on
        // any monitored pipe for this long. Deliberately generous; tools that are
        // silent for an hour are considered hung.
        constexpr std::chrono::milliseconds kDefaultIdleTimeout = std::chrono::minutes(60);

        // After the direct child exits, remaining output is drained for at most this
        // long while a descendant still holds the pipes open; the process group is
        // then killed so a stray grandchild cannot block the caller.
        constexpr std::chrono::milliseconds kExitDrainGrace = std::chrono::seconds(5);

        // Argument placeholder replaced by the extra channel identifier: the fixed
        // descriptor number (kExtraChannelFd) on POSIX, the decimal HANDLE value of
        // the inherited write end on Windows.
        constexpr const char *kExtraChannelPlaceholder = "{ddb:extra-channel}";

#ifndef _WIN32
        // Descriptor number of the extra channel write end inside the child.
        constexpr int kExtraChannelFd = 3;
#endif

        struct Options
        {
            // Per-child environment overrides (name, value). Each replaces an
            // inherited variable with the same name (case-insensitive on Windows)
            // or is added. Empty: the child inherits the environment unchanged.
            std::vector<std::pair<std::string, std::string>> envOverrides;

            // Kill the child when no output arrives on any monitored pipe for this
            // long. Zero or negative disables the timeout.
            std::chrono::milliseconds idleTimeout = kDefaultIdleTimeout;

            // Kill the child when it has run this long in total, output or not (a
            // tool that loops while printing warnings never trips idleTimeout).
            // Zero or negative disables the limit (the default).
            std::chrono::milliseconds maxRuntime{0};

            // Capacity in bytes of the output ring buffer (minimum 1).
            size_t outputCapacity = kDefaultOutputCapacity;

            // Optional: called with each complete line of stdout/stderr (without the
            // terminator; '\n' and '\r' both end a line, empty lines are skipped).
            std::function<void(const std::string &)> onOutputLine;

            // When true an extra pipe is created; its write end is passed to the
            // child (see kExtraChannelPlaceholder) and every byte read from it is
            // passed to onExtraData. Activity on it resets the idle timer.
            bool extraChannel = false;
            std::function<void(const char *, size_t)> onExtraData;
        };

        struct Result
        {
            bool started = false;   // the binary was executed
            bool exited = false;    // the child terminated normally (exitCode valid)
            int exitCode = -1;
            int termSignal = 0;     // POSIX: signal that terminated the child (0 = none)
            bool timedOut = false;  // killed by the idle timeout or the maxRuntime limit
            // Killed because the work of the calling thread was canceled (cancellation.h:
            // its cancel token, DDBShutdown); run() then throws instead of returning
            bool canceled = false;
            // Drain grace expired after the direct child exited, so its remaining
            // descendants were terminated. This does not change child success.
            bool descendantsTerminated = false;
            std::string errorMessage; // spawn/wait failure description (empty otherwise)
            std::string outputTail;   // last outputCapacity bytes of stdout+stderr
            uint64_t totalBytes = 0;  // total bytes read from stdout+stderr
            std::chrono::milliseconds duration{0};

            // True when the child ran and exited with code 0 (and was not killed).
            bool success() const { return started && exited && exitCode == 0 && !timedOut && !canceled; }

            // Human-readable status: "exited with code 3", "killed by signal 9 (Killed)",
            // "killed after 60 s without output", "failed to start: <reason>".
            // Appends a diagnostic when drain-grace cleanup terminated descendants.
            std::string describe() const;

            // Last maxLen bytes of outputTail ("..." prefixed when truncated).
            std::string tail(size_t maxLen) const;

            // describe(), then ": " and tail(tailLength) when the child printed anything:
            // the text of a failure message ("exited with code 3: <last output>").
            std::string failureText(size_t tailLength) const;
        };

        // Runs binary (an explicit path; no PATH search) with args (argv[1..]).
        // argv[0] is io::toUtf8(binary).
        Result run(const fs::path &binary,
                   const std::vector<std::string> &args,
                   const Options &options = Options());

        // The seam of the tool runners (Obj2Tiles, Untwine, build-lod, dwg2dxf): they start their
        // child through this interface, so a test substitutes a fake that returns the Result it
        // wants (an exit code, a timeout, a missing binary) or throws CanceledException, and
        // checks the arguments and options it was given, without running a tool. Same contract
        // as process::run().
        class IProcessRunner
        {
          public:
            virtual ~IProcessRunner() = default;
            virtual Result run(const fs::path &binary,
                               const std::vector<std::string> &args,
                               const Options &options) const = 0;
        };

        // The real one: process::run().
        class SystemProcessRunner final : public IProcessRunner
        {
          public:
            Result run(const fs::path &binary,
                       const std::vector<std::string> &args,
                       const Options &options) const override;
        };

        // A shared SystemProcessRunner, the default of the runners.
        const IProcessRunner &systemProcessRunner();

        // Command line for logging, quoted as the child will see it on Windows.
        std::string formatCommandLine(const fs::path &binary,
                                      const std::vector<std::string> &args);

    } // namespace process

} // namespace ddb

#endif // PROCESS_RUNNER_H
