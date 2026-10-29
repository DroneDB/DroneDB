/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "helper_pool.h"

#include <algorithm>
#include <system_error>
#include <utility>

#include "cancellation.h"
#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "process_config.h"

namespace ddb
{
    namespace helper
    {

        namespace
        {
            using Clock = std::chrono::steady_clock;

            // How often a caller that waits (for a helper, for a response) looks whether its work
            // was canceled, as process::run does while it waits for a child
            constexpr std::chrono::milliseconds kCancelSlice(500);

            // How long a helper that closed its output gets to be seen exited, so the log
            // reports how it ended instead of the kill that follows
            constexpr std::chrono::milliseconds kExitGrace(500);

            // What of the helper's stderr goes to the log: enough for the end of a sanitizer
            // report (its summary and the last frames) when a helper ends badly
            constexpr size_t kStderrInLog = 2000;
            // What of the output of the version run goes to an error message
            constexpr size_t kVersionOutputInMessage = 300;
            // The longest line the version run may print, and what of its output is kept
            constexpr size_t kVersionOutputLimit = 4096;

            size_t defaultLimit()
            {
                return ProcessConfig::get().cpuPoolSize() + 1;
            }

            // "30 s", or "250 ms" for a span under a second
            std::string describeSpan(std::chrono::milliseconds span)
            {
                if (span < std::chrono::seconds(1))
                    return std::to_string(std::max<long long>(0, span.count())) + " ms";
                return std::to_string((span + std::chrono::milliseconds(500)) / std::chrono::seconds(1)) + " s";
            }

            // "; stderr: <the end of it>" when the process wrote to stderr, for the log
            std::string stderrNote(const process::Session &session)
            {
                const std::string tail = session.stderrTail();
                return tail.empty() ? std::string() : "; stderr: " + lastBytes(tail, kStderrInLog);
            }

            // How a process that has ended ended, and what it said on stderr, for the log
            std::string endOf(const process::Session &session)
            {
                return session.describeExit() + stderrNote(session);
            }

            bool endedWell(const process::Session &session)
            {
                const auto status = session.exitStatus();
                return status.exited && status.exitCode == 0;
            }

            // Ends the wait of a caller whose work cancel::isCanceled() says is canceled, with the
            // exception the cancellation names
            [[noreturn]] void throwCanceled()
            {
                cancel::throwIfCanceled();
                throw CanceledException("canceled");
            }

            // The next line of the process' output, waited for until `deadline` in slices, so a
            // caller whose work is canceled does not wait for it: empty then (the process is in
            // the middle of something, the caller kills it). Timeout once the deadline passed.
            std::optional<process::Session::ReadResult> readLineUnlessCanceled(process::Session &session,
                                                                               Clock::time_point deadline,
                                                                               size_t maxBytes)
            {
                for (;;)
                {
                    auto line = session.readLine(std::min(deadline, Clock::now() + kCancelSlice), maxBytes);
                    if (line.status != process::Session::ReadStatus::Timeout || Clock::now() >= deadline)
                        return line;
                    if (cancel::isCanceled())
                        return std::nullopt;
                }
            }

            // What tells one file from another to the version cache: a program replaced or
            // rebuilt in place has another size or time. Empty when the file cannot be read.
            struct FileIdentity
            {
                uintmax_t size = 0;
                fs::file_time_type modified;

                bool operator==(const FileIdentity &other) const
                {
                    return size == other.size && modified == other.modified;
                }
            };

            std::optional<FileIdentity> identityOf(const fs::path &program)
            {
                std::error_code ec;
                FileIdentity identity;
                identity.size = fs::file_size(program, ec);
                if (ec)
                    return std::nullopt;
                identity.modified = fs::last_write_time(program, ec);
                if (ec)
                    return std::nullopt;
                return identity;
            }
        } // namespace

        std::string lastBytes(const std::string &text, size_t length)
        {
            std::string tail = text.size() > length ? "..." + text.substr(text.size() - length) : text;
            while (!tail.empty() && (tail.back() == '\n' || tail.back() == '\r' || tail.back() == ' '))
                tail.pop_back();
            return tail;
        }

        // A program whose version was accepted, as it was on disk then
        struct Pool::VerifiedProgram
        {
            fs::path program;
            FileIdentity identity;
            std::string version;
        };

        // A running helper
        struct Pool::Helper
        {
            process::Session session;
            fs::path program;
            std::string version;
            Clock::time_point lastUsed;
        };

        // ---- the lease

        Pool::Lease::Lease(Pool &pool, std::unique_ptr<Helper> helper, uint64_t generation)
            : pool_(&pool), helper_(std::move(helper)), generation_(generation)
        {
        }

        Pool::Lease::Lease(Lease &&other) noexcept
            : pool_(std::exchange(other.pool_, nullptr)),
              helper_(std::move(other.helper_)),
              generation_(other.generation_),
              keep_(other.keep_),
              context_(std::move(other.context_))
        {
        }

        Pool::Lease::~Lease()
        {
            if (pool_ != nullptr)
                pool_->release(std::move(helper_), keep_, generation_);
        }

        std::unique_ptr<Pool::Helper> Pool::Lease::discard()
        {
            keep_ = false;
            return std::move(helper_);
        }

        void Pool::Lease::install(std::unique_ptr<Helper> helper)
        {
            helper_ = std::move(helper);
        }

        unsigned long Pool::Lease::pid() const
        {
            return helper_ ? helper_->session.pid() : 0;
        }

        void Pool::Lease::fail(const std::string &reason, bool mayHaveEnded)
        {
            const std::unique_ptr<Helper> helper = discard();
            process::Session &session = helper->session;
            if (mayHaveEnded)
                session.wait(Clock::now() + kExitGrace);
            session.kill();

            LOGW << "Discarded " << pool_->options_.name << " (pid " << session.pid() << ")"
                 << (context_.empty() ? std::string() : " " + context_) << ": " << reason << "; it "
                 << endOf(session);
            throw Failure(reason);
        }

        void Pool::Lease::abandon(const std::string &why) noexcept
        {
            try
            {
                const std::unique_ptr<Helper> helper = discard();
                if (!helper)
                    return;
                helper->session.kill();
                LOGD << "Discarded " << pool_->options_.name << " (pid " << helper->session.pid() << ")"
                     << (context_.empty() ? std::string() : " " + context_) << ": " << why;
            }
            catch (...)
            {
                // A caller cannot do more; the session kills the helper when it goes
            }
        }

        void Pool::Lease::send(const std::string &line, Clock::time_point deadline)
        {
            const std::string &name = pool_->options_.name;
            process::Session &session = helper_->session;

            const auto written = session.writeLine(line, deadline);
            pool_->requests_++;
            if (written == process::Session::WriteStatus::Closed)
                fail(name + " is gone (its input is closed)", true);
            if (written == process::Session::WriteStatus::Timeout)
                fail(name + " did not read the request in time", false);
        }

        std::string Pool::Lease::receive(Clock::time_point deadline)
        {
            using Status = process::Session::ReadStatus;
            const std::string &name = pool_->options_.name;
            const size_t maxBytes = pool_->options_.maxResponseBytes;
            process::Session &session = helper_->session;

            const auto allowed =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            const auto read = readLineUnlessCanceled(session, deadline, maxBytes);
            if (!read)
            {
                // The helper is in the middle of a request: it cannot be reused
                discard()->session.kill();
                throwCanceled();
            }
            switch (read->status)
            {
            case Status::Timeout:
                fail(name + " gave no response in " + describeSpan(allowed), false);
            case Status::Eof:
                fail(name + " closed its output without answering", true);
            case Status::TooLong:
                fail(name + " sent a response longer than " + std::to_string(maxBytes / 1024) + " KiB", false);
            case Status::Line:
                break;
            }
            return read->line;
        }

        std::string Pool::Lease::exchange(const std::string &line, Clock::time_point deadline)
        {
            send(line, deadline);
            return receive(deadline);
        }

        // ---- the pool

        Pool::Pool(PoolOptions options)
            : options_(std::move(options)),
              limit_(options_.limit ? options_.limit : std::function<size_t()>(defaultLimit)),
              now_(options_.now ? options_.now : std::function<Clock::time_point()>(&Clock::now))
        {
            if (!options_.locate)
                throw std::invalid_argument("A helper pool needs a way to find its program");
        }

        Pool::~Pool()
        {
            close();
        }

        void Pool::unavailable(const std::string &message) const
        {
            if (options_.raiseUnavailable)
                options_.raiseUnavailable(message);
            throw HelperUnavailableException(message);
        }

        fs::path Pool::locateProgram() const
        {
            fs::path program = options_.locate();
            if (program.empty())
                unavailable(options_.missingMessage ? options_.missingMessage() : options_.name + " was not found");
            return program;
        }

        void Pool::collectRetired(const fs::path &program, HelperList &retired)
        {
            const auto now = now_();
            for (auto it = idle_.begin(); it != idle_.end();)
            {
                const bool otherProgram = !program.empty() && (*it)->program != program;
                const bool tooOld = now - (*it)->lastUsed > options_.idleTimeout;
                if (otherProgram || tooOld)
                {
                    retired.push_back(std::move(*it));
                    it = idle_.erase(it);
                }
                else
                    ++it;
            }
        }

        size_t Pool::currentLimit() const
        {
            return std::max<size_t>(1, limit_());
        }

        bool Pool::hasRoom() const
        {
            return !idle_.empty() || busy_ + idle_.size() < currentLimit();
        }

        Pool::Lease Pool::acquire()
        {
            std::optional<Lease> lease = take(true);
            return std::move(*lease);
        }

        std::optional<Pool::Lease> Pool::tryAcquire()
        {
            return take(false);
        }

        std::optional<Pool::Lease> Pool::take(bool waitTurn)
        {
            const fs::path program = locateProgram();

            HelperList retired;
            std::unique_ptr<Helper> helper;
            uint64_t generation;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (waitTurn)
                {
                    const uint64_t ticket = nextTicket_++;
                    queue_.push_back(ticket);
                    changed_.notify_all();
                    try
                    {
                        // Only the caller at the head of the queue may take a helper or start one:
                        // callers are served in the order they came
                        for (;;)
                        {
                            if (closed_)
                                unavailable(options_.name + " is not available: its pool was closed");
                            collectRetired(program, retired);
                            if (queue_.front() == ticket && hasRoom())
                                break;
                            changed_.wait_for(lock, kCancelSlice);
                            if (cancel::isCanceled())
                                cancel::throwIfCanceled();
                        }
                    }
                    catch (...)
                    {
                        queue_.erase(std::find(queue_.begin(), queue_.end(), ticket));
                        changed_.notify_all();
                        throw;
                    }
                    queue_.pop_front();
                }
                else
                {
                    if (closed_)
                        unavailable(options_.name + " is not available: its pool was closed");
                    collectRetired(program, retired);
                    // The callers in the queue came first: a caller that does not wait does not
                    // pass them, even when a helper is free for the moment (one was returned and
                    // the head of the queue has not woken yet)
                    if (!queue_.empty() || !hasRoom())
                    {
                        lock.unlock();
                        retire(retired);
                        return std::nullopt;
                    }
                }
                if (!idle_.empty())
                {
                    helper = std::move(idle_.back());
                    idle_.pop_back();
                }
                busy_++;
                generation = generation_;
                changed_.notify_all();
            }

            Lease lease(*this, std::move(helper), generation);
            retire(retired);
            // A helper can end while it waits idle (the OOM killer, a user's kill): that is not
            // the fault of the request the caller is about to make, so it takes a new helper in
            // the place of the dead one. The look costs no wait: has the process been reaped?
            if (lease.helper() != nullptr && lease.helper()->session.wait(Clock::now()))
            {
                const std::unique_ptr<Helper> dead = lease.discard();
                LOGW << "Discarded " << options_.name << " (pid " << dead->session.pid()
                     << "), which ended while it was idle: it " << endOf(dead->session);
            }
            if (lease.helper() == nullptr)
                lease.install(startHelper(program));
            return std::optional<Lease>(std::move(lease));
        }

        std::string Pool::verifiedVersion(const fs::path &program)
        {
            // One check at a time: callers that start helpers of the same program together wait
            // for the first check instead of running their own, and see the cancellation while
            // they wait
            std::unique_lock<std::timed_mutex> lock(versionMutex_, std::defer_lock);
            while (!lock.try_lock_for(kCancelSlice))
                cancel::throwIfCanceled();

            const auto identity = identityOf(program);
            if (identity)
                for (const auto &verified : verified_)
                    if (verified.program == program && verified.identity == *identity)
                        return verified.version;

            // Only an accepted version is kept: a refused program is asked again next time (it
            // may have been fixed meanwhile)
            const std::string version = checkVersion(program);
            if (identity)
            {
                verified_.erase(std::remove_if(verified_.begin(), verified_.end(),
                                               [&](const VerifiedProgram &verified) { return verified.program == program; }),
                                verified_.end());
                verified_.push_back({program, *identity, version});
            }
            return version;
        }

        std::string Pool::checkVersion(const fs::path &program) const
        {
            using Status = process::Session::ReadStatus;
            const std::string where = options_.name + " at " + io::toUtf8(program);
            const std::string timeout = describeSpan(options_.versionTimeout);
            const std::string versionCommand = options_.versionArgs.empty() ? "version check" : options_.versionArgs.front();

            // A session, not process::run: it learns of the exit from the process itself and so
            // ends as soon as the program does, with nothing to poll for (a check sits on the
            // path of the first request of every helper)
            process::Session session;
            if (!session.start(program, options_.versionArgs, options_.session))
                unavailable("Cannot use " + where + ": " + session.startError());

            const auto deadline = Clock::now() + options_.versionTimeout;
            std::string output;
            for (bool more = true; more;)
            {
                const auto line = readLineUnlessCanceled(session, deadline, kVersionOutputLimit);
                if (!line)
                {
                    session.kill();
                    throwCanceled();
                }
                switch (line->status)
                {
                case Status::Line:
                    output += line->line + '\n';
                    if (output.size() > kVersionOutputLimit)
                        output.erase(0, output.size() - kVersionOutputLimit);
                    break;
                case Status::Eof:
                    output += line->line;
                    more = false;
                    break;
                case Status::TooLong:
                    unavailable("Cannot use " + where + ": its " + versionCommand + " printed a line longer than " +
                                std::to_string(kVersionOutputLimit) + " bytes");
                case Status::Timeout:
                    unavailable("Cannot use " + where + ": its " + versionCommand + " did not answer in " + timeout);
                }
            }
            while (!session.wait(std::min(deadline, Clock::now() + kCancelSlice)))
            {
                if (Clock::now() >= deadline)
                    unavailable("Cannot use " + where + ": its " + versionCommand + " did not end in " + timeout);
                if (cancel::isCanceled())
                {
                    session.kill();
                    throwCanceled();
                }
            }
            if (!endedWell(session))
                unavailable("Cannot use " + where + ": its " + versionCommand + " " + endOf(session));

            if (!options_.validateVersion)
                return lastBytes(output, kVersionOutputInMessage);
            const VersionVerdict verdict = options_.validateVersion(output);
            if (!verdict.accepted)
                unavailable("Cannot use " + where + ": " + verdict.text);
            return verdict.text;
        }

        std::unique_ptr<Pool::Helper> Pool::startHelper(const fs::path &program)
        {
            // A caller whose work is canceled starts no process (its place goes back with the
            // lease)
            cancel::throwIfCanceled();
            const std::string version = verifiedVersion(program);

            auto helper = std::make_unique<Helper>();
            helper->program = program;
            helper->version = version;
            if (!helper->session.start(program, options_.serveArgs, options_.session))
                unavailable("Cannot start " + options_.name + " at " + io::toUtf8(program) + ": " +
                            helper->session.startError());
            started_++;
            LOGD << "Started " << options_.name << " " << io::toUtf8(program) << " (pid " << helper->session.pid()
                 << ", " << version << ")";
            return helper;
        }

        void Pool::release(std::unique_ptr<Helper> helper, bool keep, uint64_t generation) noexcept
        {
            try
            {
                HelperList retired;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    busy_--;
                    if (helper)
                    {
                        if (keep && !closed_ && generation == generation_ && busy_ + idle_.size() < currentLimit())
                        {
                            helper->lastUsed = now_();
                            idle_.push_back(std::move(helper));
                        }
                        else
                            retired.push_back(std::move(helper));
                    }
                    collectRetired(fs::path(), retired);
                    changed_.notify_all();
                }
                retire(retired);
            }
            catch (...)
            {
                // A destructor cannot report; the helpers are killed when their sessions go
            }
        }

        size_t Pool::retire(HelperList &helpers) const
        {
            for (const auto &helper : helpers)
                helper->session.closeStdin();
            const auto deadline = Clock::now() + options_.closeTimeout;
            for (const auto &helper : helpers)
                helper->session.wait(deadline);
            for (const auto &helper : helpers)
            {
                const process::Session &session = helper->session;
                if (!session.finished())
                {
                    helper->session.kill();
                    LOGW << options_.name << " (pid " << session.pid() << ") did not end "
                         << std::chrono::duration_cast<std::chrono::milliseconds>(options_.closeTimeout).count()
                         << " ms after its input was closed: killed" << stderrNote(session);
                }
                else if (!endedWell(session))
                    // A helper reports what it finds at exit through its status: a sanitizer
                    // build exits non-zero on a leak, and this is where that shows
                    LOGW << options_.name << " (pid " << session.pid() << ") did not end well after its input was closed: it "
                         << endOf(session);
                else
                    LOGD << "Closed " << options_.name << " (pid " << session.pid() << ")";
            }
            const size_t closed = helpers.size();
            helpers.clear();
            return closed;
        }

        size_t Pool::drain(bool closing)
        {
            HelperList retired;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                generation_++;
                if (closing)
                    closed_ = true;
                retired = std::move(idle_);
                idle_.clear();
                changed_.notify_all();
            }
            return retire(retired);
        }

        size_t Pool::shutdown()
        {
            return drain(false);
        }

        size_t Pool::close()
        {
            return drain(true);
        }

        // ---- what the tests observe

        size_t Pool::idleHelpers() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return idle_.size();
        }

        size_t Pool::busyHelpers() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return busy_;
        }

        size_t Pool::queuedCallers() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return queue_.size();
        }

        bool Pool::awaitWaiters(size_t count, std::chrono::milliseconds timeout) const
        {
            std::unique_lock<std::mutex> lock(mutex_);
            return changed_.wait_for(lock, timeout, [&] { return queue_.size() >= count; });
        }

        std::vector<unsigned long> Pool::idleHelperPids() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::vector<unsigned long> pids;
            for (const auto &helper : idle_)
                pids.push_back(helper->session.pid());
            return pids;
        }

    } // namespace helper
} // namespace ddb
