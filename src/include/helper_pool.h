/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef HELPER_POOL_H
#define HELPER_POOL_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "fs.h"
#include "process_runner.h"

namespace ddb
{
    namespace helper
    {
        // A request broke its helper: thrown by Lease::send(), receive(), exchange() and fail()
        // once the helper is killed, dropped from the pool and logged. The caller maps it to the
        // error of its own operation; the other helpers of the pool do not notice.
        class Failure : public std::runtime_error
        {
          public:
            using std::runtime_error::runtime_error;
        };

        // The last `length` bytes of a program's output, with "..." in front when text was cut and
        // the trailing line ends and spaces removed: how outputs go to messages and the log
        std::string lastBytes(const std::string &text, size_t length);

        // What a version validator says of the output of `<program> --version`
        struct VersionVerdict
        {
            bool accepted = false;
            // Accepted: the version as the log shows it ("1.0"). Refused: why, as the end of
            // "Cannot use <program> at <path>: ..." ("it speaks protocol 2.0, this ddb needs 1.x")
            std::string text;
        };

        // What a pool is told. `name` and `locate` are required; the rest has the defaults given.
        struct PoolOptions
        {
            using Clock = std::chrono::steady_clock;

            // How messages and the log call the program ("metadata-util")
            std::string name;
            // The executable, empty when there is none. Asked whenever a caller takes a helper,
            // so a program installed or replaced meanwhile counts at once.
            std::function<fs::path()> locate;
            // Says what is missing when locate() found nothing (how to install it, which
            // variable names it); default: "<name> was not found"
            std::function<std::string()> missingMessage;
            // The arguments that make the program print its version and end, and those that make
            // it serve requests (lines on its stdin, lines on its stdout) until its stdin ends
            std::vector<std::string> versionArgs = {"--version"};
            std::vector<std::string> serveArgs = {"--serve"};
            // The accept rule of the protocol, applied to what the version run printed. Empty:
            // any output is accepted (as its own text).
            std::function<VersionVerdict(const std::string &output)> validateVersion;
            // Throws the exception of the caller's family for "the helper cannot be used"
            // (a missing program, a refused version, a start that fails). Default and fallback:
            // HelperUnavailableException.
            std::function<void(const std::string &message)> raiseUnavailable;
            // How every helper (and its version run) starts: environment overrides, memory limit
            process::SessionOptions session;

            // The most helpers at once (held plus idle). Default: ProcessConfig::cpuPoolSize() + 1,
            // read whenever a caller takes one, so DDBConfigure applies at once.
            std::function<size_t()> limit;
            // The time the idle limit counts in. Default: the steady clock
            std::function<Clock::time_point()> now;

            // A helper idle for longer is closed
            std::chrono::milliseconds idleTimeout = std::chrono::seconds(60);
            // To start the version run and have it answer and end
            std::chrono::milliseconds versionTimeout = std::chrono::seconds(10);
            // The time all helpers closed together get to end after their input is closed
            std::chrono::milliseconds closeTimeout = std::chrono::seconds(2);
            // A longer response line is a broken helper
            size_t maxResponseBytes = 16 * 1024 * 1024;
        };

        // A pool of helper processes that speak in lines: a request line on the helper's stdin,
        // a response line on its stdout, one request at a time (process::Session).
        //
        //   - A helper is held by one caller at a time (a Lease); between requests it waits idle
        //     in the pool.
        //   - The pool holds at most limit() helpers. A caller that finds none idle and the pool
        //     full waits its turn: callers are served in the order they arrived. tryAcquire()
        //     does not wait.
        //   - A helper is started only when a caller needs one and none is idle, after
        //     `<program> --version` printed what the validator accepts. The verdict is kept for the
        //     program file as it is (path, size, modification time): the program is asked once,
        //     and again when the file changes. A refused program is asked again every time.
        //   - An idle helper that has ended by itself (killed by the system or a user) is
        //     dropped when a caller takes it, and the caller gets a new one.
        //   - The program is looked up whenever a caller takes a helper; idle helpers of another
        //     program are closed.
        //   - An idle helper is closed when it has been idle for more than idleTimeout; the check
        //     runs when a helper is taken or returned (no timer thread).
        //   - A helper that dies, hangs, sends a line longer than the limit, or whose caller says
        //     it sent what the protocol does not allow (Lease::fail) is killed and dropped, and the
        //     request fails. A killed helper is never put back. The next request starts a new one.
        //   - A helper closed by the pool that does not exit with 0 (a sanitizer build reports a
        //     leak that way) is a WARNING in the log, with its status and the end of its stderr.
        //   - A caller whose work is canceled (cancellation.h) stops waiting for a turn, for the
        //     version run and for a response within half a second; the helper in the middle of a
        //     request is killed.
        //
        // The pool never holds its lock while it starts a process or talks to one.
        class Pool final
        {
          public:
            using Clock = std::chrono::steady_clock;

            explicit Pool(PoolOptions options);
            // Closes the idle helpers. Helpers held by callers must not exist (no pool is
            // destroyed while it is used)
            ~Pool();

            Pool(const Pool &) = delete;
            Pool &operator=(const Pool &) = delete;

            class Lease;

            // Takes a helper (idle, or a new one) for the caller, waiting its turn.
            // @throws HelperUnavailableException (or what raiseUnavailable throws) when the program
            // is missing, refused or does not start, and after close(); CanceledException when the
            // caller's work is canceled meanwhile.
            Lease acquire();
            // The same without waiting: empty when the caller would wait, because no helper is
            // idle and the pool is full, or because callers are queued (they came first)
            std::optional<Lease> tryAcquire();

            // Closes the idle helpers: their input first, then one wait of at most closeTimeout
            // for all of them, then a kill for the ones still there. Helpers held by a caller
            // are closed when it returns them. The pool can be used again afterwards. Returns the
            // number of helpers closed.
            size_t shutdown();
            // shutdown(), and the pool takes no caller any more: acquire() and tryAcquire() throw,
            // and callers that wait for a turn stop waiting with the same exception.
            size_t close();

            // The most helpers right now (limit(), at least 1)
            size_t currentLimit() const;

            // What the tests observe the pool through: how many processes it started and how
            // many request lines it wrote over the life of the object, the helpers waiting and
            // the helpers held right now, the callers queued for a helper, and the process ids of
            // the idle helpers. awaitWaiters() blocks until `count` callers are queued (false
            // after the timeout), so a test queues callers in a known order without sleeping.
            size_t startedHelpers() const { return started_.load(); }
            size_t requestsSent() const { return requests_.load(); }
            size_t idleHelpers() const;
            size_t busyHelpers() const;
            size_t queuedCallers() const;
            bool awaitWaiters(size_t count, std::chrono::milliseconds timeout) const;
            std::vector<unsigned long> idleHelperPids() const;

          private:
            struct Helper;
            struct VerifiedProgram;

            using HelperList = std::vector<std::unique_ptr<Helper>>;

            // What both acquire functions do; `waitTurn`: queue and wait instead of returning empty
            std::optional<Lease> take(bool waitTurn);
            // Under the lock: a helper is idle, or the limit leaves room to start one
            bool hasRoom() const;
            // Under the lock: moves the idle helpers that are too old, or of another program than
            // `program`, to `retired`
            void collectRetired(const fs::path &program, HelperList &retired);
            fs::path locateProgram() const;
            std::unique_ptr<Helper> startHelper(const fs::path &program);
            // checkVersion() of `program`, once for the file as it is now
            std::string verifiedVersion(const fs::path &program);
            // What `program --version` printed, as the validator accepts it
            std::string checkVersion(const fs::path &program) const;
            [[noreturn]] void unavailable(const std::string &message) const;

            // Gives back a helper the caller held (keep: it is healthy and goes idle if the pool
            // has room and no shutdown() happened since the caller took it, `generation`), or
            // only its place when there is none
            void release(std::unique_ptr<Helper> helper, bool keep, uint64_t generation) noexcept;
            // Closes helpers that are out of the pool; returns how many
            size_t retire(HelperList &helpers) const;

            // shutdown() and close()
            size_t drain(bool closing);

            const PoolOptions options_;
            const std::function<size_t()> limit_;
            const std::function<Clock::time_point()> now_;

            mutable std::mutex mutex_;
            mutable std::condition_variable changed_;
            // The oldest first: callers take the newest
            HelperList idle_;
            // Callers hold a helper, or the place for one they are starting
            size_t busy_ = 0;
            // The callers waiting for a helper, first come first served
            std::deque<uint64_t> queue_;
            uint64_t nextTicket_ = 0;
            // Helpers of an older generation (before a shutdown()) are closed when returned
            uint64_t generation_ = 0;
            bool closed_ = false;

            // The programs whose version was accepted; held while a version is checked
            std::timed_mutex versionMutex_;
            std::vector<VerifiedProgram> verified_;

            std::atomic<size_t> started_{0};
            std::atomic<size_t> requests_{0};
        };

        // The place of one helper in the pool, and the helper once there is one: a caller holds it
        // from the moment it was granted a place, so the place is given back on every path out,
        // exceptions included (a failing start, a canceled run of the version check). The helper
        // goes back to the pool only when the caller says it is still good (keep()), so a
        // response the caller did not accept never reaches the next caller.
        class Pool::Lease
        {
          public:
            Lease(Lease &&other) noexcept;
            ~Lease();
            Lease(const Lease &) = delete;
            Lease &operator=(const Lease &) = delete;
            Lease &operator=(Lease &&) = delete;

            // What the log says the helper was doing when it is discarded ("reading /a/b.jpg")
            void setContext(std::string context) { context_ = std::move(context); }

            // Writes one request line, waiting for the helper to read it until `deadline`.
            // @throws Failure when the helper is gone or does not read in time (it is killed)
            void send(const std::string &line, Clock::time_point deadline);
            // Reads one response line (without its terminator), waiting for it until `deadline`.
            // The wait looks every 500 ms whether the caller's work was canceled: then the helper is
            // killed and CanceledException is thrown.
            // @throws Failure when the helper does not answer in time, closes its output or sends a
            // line longer than maxResponseBytes (it is killed)
            std::string receive(Clock::time_point deadline);
            // send() and receive() with one deadline for both
            std::string exchange(const std::string &line, Clock::time_point deadline);

            // The caller found the response unusable: the helper is killed, dropped and logged
            // with `reason`, and Failure(reason) is thrown. `mayHaveEnded`: the helper may have
            // exited by itself, so its exit status is worth a short wait.
            [[noreturn]] void fail(const std::string &reason, bool mayHaveEnded);
            // The helper answered well: it goes back to the pool when the lease ends
            void keep() { keep_ = true; }
            // Kills and drops the helper: the caller stops using it without reading its response
            // (the helper may hang, and nobody may reuse it while its response is unread)
            void abandon(const std::string &why) noexcept;

            unsigned long pid() const;

          private:
            friend class Pool;

            Lease(Pool &pool, std::unique_ptr<Helper> helper, uint64_t generation);

            // The helper is out of the pool for good; the caller closes it
            std::unique_ptr<Helper> discard();
            void install(std::unique_ptr<Helper> helper);
            Helper *helper() const { return helper_.get(); }

            Pool *pool_;
            std::unique_ptr<Helper> helper_;
            uint64_t generation_;
            bool keep_ = false;
            std::string context_;
        };

    } // namespace helper
} // namespace ddb

#endif // HELPER_POOL_H
