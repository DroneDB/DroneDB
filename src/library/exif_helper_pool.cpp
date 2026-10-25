/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "exif_helper_pool.h"

#include <algorithm>
#include <exception>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "cancellation.h"
#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "process_config.h"
#include "process_runner.h"
#include "tool_locator.h"

namespace ddb
{
    namespace exif
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
            // What of the output of `metadata-util --version` goes to an error message
            constexpr size_t kVersionOutputInMessage = 300;
            // The longest line `metadata-util --version` may print, and what of its output is kept
            constexpr size_t kVersionOutputLimit = 4096;

            std::string lastBytes(const std::string &text, size_t length)
            {
                std::string tail = text.size() > length ? "..." + text.substr(text.size() - length) : text;
                while (!tail.empty() && (tail.back() == '\n' || tail.back() == '\r' || tail.back() == ' '))
                    tail.pop_back();
                return tail;
            }

            std::string missingProgramMessage()
            {
                const std::string hint = std::string("set ") + kMetadataUtilEnv + " to the " + kMetadataUtilName +
                                         " executable, or install it next to ddb or on the PATH";
                if (const auto configured = io::getEnvUtf8(kMetadataUtilEnv); configured && !configured->empty())
                    return std::string(kMetadataUtilEnv) + "=" + *configured + " is not an executable file, so " +
                           kMetadataUtilName + " cannot be used to read image metadata; " + hint;
                return std::string(kMetadataUtilName) + " was not found, and it is needed to read image metadata; " +
                       hint;
            }

            fs::path locateByTool()
            {
                fs::path program = tools::findTool(kMetadataUtilName, kMetadataUtilEnv);
                // The answer is cached; a program installed since is worth one more look
                if (program.empty())
                    program = tools::findTool(kMetadataUtilName, kMetadataUtilEnv, true);
                return program;
            }

            size_t defaultLimit()
            {
                return ProcessConfig::get().cpuPoolSize() + 1;
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

        // A program whose `--version` was accepted, as it was on disk then
        struct HelperMetadataReader::VerifiedProgram
        {
            fs::path program;
            FileIdentity identity;
            protocol::Version version;
        };

        // A running metadata-util
        struct HelperMetadataReader::Helper
        {
            process::Session session;
            fs::path program;
            protocol::Version version;
            Clock::time_point lastUsed;
        };

        // A request broke its helper: thrown by serve() once the helper is gone
        struct HelperMetadataReader::Failure : std::runtime_error
        {
            using std::runtime_error::runtime_error;
        };

        // The place of one helper in the pool, and the helper once there is one: a caller holds it
        // from the moment it was granted a place, so the place is given back on every path out,
        // exceptions included (a failing start, a canceled run of `--version`). The helper goes
        // back to the pool only when the caller says it is still good.
        class HelperMetadataReader::Lease
        {
          public:
            Lease(HelperMetadataReader &pool, std::unique_ptr<Helper> helper, uint64_t generation)
                : pool_(&pool), helper_(std::move(helper)), generation_(generation)
            {
            }
            Lease(Lease &&other) noexcept
                : pool_(std::exchange(other.pool_, nullptr)),
                  helper_(std::move(other.helper_)),
                  generation_(other.generation_),
                  keep_(other.keep_)
            {
            }
            Lease(const Lease &) = delete;
            Lease &operator=(const Lease &) = delete;
            Lease &operator=(Lease &&) = delete;

            ~Lease()
            {
                if (pool_ != nullptr)
                    pool_->release(std::move(helper_), keep_, generation_);
            }

            Helper *helper() const { return helper_.get(); }
            void install(std::unique_ptr<Helper> helper) { helper_ = std::move(helper); }
            // The helper answered well: it goes back to the pool
            void keep() { keep_ = true; }
            // The helper is out of the pool for good; the caller closes it
            std::unique_ptr<Helper> discard()
            {
                keep_ = false;
                return std::move(helper_);
            }

          private:
            HelperMetadataReader *pool_;
            std::unique_ptr<Helper> helper_;
            uint64_t generation_;
            bool keep_ = false;
        };

        HelperMetadataReader::HelperMetadataReader(Options options)
            : options_(std::move(options)),
              locate_(options_.locate ? options_.locate : std::function<fs::path()>(locateByTool)),
              limit_(options_.limit ? options_.limit : std::function<size_t()>(defaultLimit)),
              now_(options_.now ? options_.now : std::function<Clock::time_point()>(&Clock::now))
        {
        }

        HelperMetadataReader::~HelperMetadataReader()
        {
            shutdown();
        }

        namespace
        {
            std::atomic<HelperMetadataReader *> g_instance{nullptr};
        } // namespace

        HelperMetadataReader &HelperMetadataReader::instance()
        {
            // Never destroyed (see the header)
            static HelperMetadataReader *const reader = [] {
                auto *created = new HelperMetadataReader();
                g_instance.store(created, std::memory_order_release);
                return created;
            }();
            return *reader;
        }

        size_t HelperMetadataReader::shutdownInstance()
        {
            HelperMetadataReader *reader = g_instance.load(std::memory_order_acquire);
            return reader != nullptr ? reader->shutdown() : 0;
        }

        // ---- the pool

        fs::path HelperMetadataReader::locateProgram() const
        {
            fs::path program = locate_();
            if (program.empty())
                throw MetadataHelperException(missingProgramMessage());
            return program;
        }

        void HelperMetadataReader::collectRetired(const fs::path &program, HelperList &retired)
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

        bool HelperMetadataReader::hasRoom() const
        {
            const size_t limit = std::max<size_t>(1, limit_());
            return !idle_.empty() || busy_ + idle_.size() < limit;
        }

        HelperMetadataReader::Lease HelperMetadataReader::acquire()
        {
            std::optional<Lease> lease = take(true);
            return std::move(*lease);
        }

        std::optional<HelperMetadataReader::Lease> HelperMetadataReader::tryAcquire()
        {
            return take(false);
        }

        std::optional<HelperMetadataReader::Lease> HelperMetadataReader::take(bool waitTurn)
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
            // the fault of the file the caller is about to read, so it takes a new helper in
            // the place of the dead one. The look costs no wait: has the process been reaped?
            if (lease.helper() != nullptr && lease.helper()->session.wait(Clock::now()))
            {
                const std::unique_ptr<Helper> dead = lease.discard();
                LOGW << "Discarded " << kMetadataUtilName << " (pid " << dead->session.pid()
                     << "), which ended while it was idle: it " << endOf(dead->session);
            }
            if (lease.helper() == nullptr)
                lease.install(startHelper(program));
            return std::optional<Lease>(std::move(lease));
        }

        protocol::Version HelperMetadataReader::verifiedVersion(const fs::path &program)
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
            const protocol::Version version = checkVersion(program);
            if (identity)
            {
                verified_.erase(std::remove_if(verified_.begin(), verified_.end(),
                                               [&](const VerifiedProgram &verified) { return verified.program == program; }),
                                verified_.end());
                verified_.push_back({program, *identity, version});
            }
            return version;
        }

        protocol::Version HelperMetadataReader::checkVersion(const fs::path &program) const
        {
            using Status = process::Session::ReadStatus;
            const std::string where = std::string(kMetadataUtilName) + " at " + io::toUtf8(program);
            const std::string timeout =
                std::to_string(std::chrono::duration_cast<std::chrono::seconds>(options_.versionTimeout).count()) + " s";

            // A session, not process::run: it learns of the exit from the process itself and so
            // ends as soon as the program does, with nothing to poll for (a check sits on the
            // path of the first read of every helper)
            process::Session session;
            if (!session.start(program, {"--version"}))
                throw MetadataHelperException("Cannot use " + where + ": " + session.startError());

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
                    throw MetadataHelperException("Cannot use " + where + ": its --version printed a line longer than " +
                                                  std::to_string(kVersionOutputLimit) + " bytes");
                case Status::Timeout:
                    throw MetadataHelperException("Cannot use " + where + ": its --version did not answer in " + timeout);
                }
            }
            while (!session.wait(std::min(deadline, Clock::now() + kCancelSlice)))
            {
                if (Clock::now() >= deadline)
                    throw MetadataHelperException("Cannot use " + where + ": its --version did not end in " + timeout);
                if (cancel::isCanceled())
                {
                    session.kill();
                    throwCanceled();
                }
            }
            if (!endedWell(session))
                throw MetadataHelperException("Cannot use " + where + ": its --version " + endOf(session));

            const auto version = protocol::parseVersionOutput(output);
            if (!version)
                throw MetadataHelperException("Cannot use " + where + ": its --version printed no protocol version (" +
                                              lastBytes(output, kVersionOutputInMessage) + ")");
            if (!protocol::isAccepted(*version))
                throw MetadataHelperException(
                    where + " speaks protocol " + protocol::toString(*version) + ", this ddb needs " +
                    protocol::toString({protocol::kMajorVersion, protocol::kMinorVersion}) + " or a later " +
                    std::to_string(protocol::kMajorVersion) + ".x; set " + kMetadataUtilEnv + " to a compatible " +
                    kMetadataUtilName);
            return *version;
        }

        std::unique_ptr<HelperMetadataReader::Helper> HelperMetadataReader::startHelper(const fs::path &program)
        {
            // A caller whose work is canceled starts no process (its place goes back with the
            // lease)
            cancel::throwIfCanceled();
            const protocol::Version version = verifiedVersion(program);

            auto helper = std::make_unique<Helper>();
            helper->program = program;
            helper->version = version;
            if (!helper->session.start(program, {"--serve"}))
                throw MetadataHelperException(std::string("Cannot start ") + kMetadataUtilName + " at " +
                                              io::toUtf8(program) + ": " + helper->session.startError());
            started_++;
            LOGD << "Started " << kMetadataUtilName << " " << io::toUtf8(program) << " (pid " << helper->session.pid()
                 << ", protocol " << protocol::toString(version) << ")";
            return helper;
        }

        void HelperMetadataReader::release(std::unique_ptr<Helper> helper, bool keep, uint64_t generation) noexcept
        {
            try
            {
                HelperList retired;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    busy_--;
                    if (helper)
                    {
                        const size_t limit = std::max<size_t>(1, limit_());
                        if (keep && generation == generation_ && busy_ + idle_.size() < limit)
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

        size_t HelperMetadataReader::retire(HelperList &helpers) const
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
                    LOGW << kMetadataUtilName << " (pid " << session.pid() << ") did not end "
                         << std::chrono::duration_cast<std::chrono::milliseconds>(options_.closeTimeout).count()
                         << " ms after its input was closed: killed" << stderrNote(session);
                }
                else if (!endedWell(session))
                    // A helper reports what it finds at exit through its status: a sanitizer
                    // build exits non-zero on a leak, and this is where that shows
                    LOGW << kMetadataUtilName << " (pid " << session.pid() << ") did not end well after its input was closed: it "
                         << endOf(session);
                else
                    LOGD << "Closed " << kMetadataUtilName << " (pid " << session.pid() << ")";
            }
            const size_t closed = helpers.size();
            helpers.clear();
            return closed;
        }

        size_t HelperMetadataReader::shutdown()
        {
            HelperList retired;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                generation_++;
                retired = std::move(idle_);
                idle_.clear();
                changed_.notify_all();
            }
            return retire(retired);
        }

        // ---- requests

        void HelperMetadataReader::fail(Lease &lease, const fs::path &file, protocol::Operation operation,
                                        const std::string &reason, bool mayHaveEnded) const
        {
            std::unique_ptr<Helper> helper = lease.discard();
            process::Session &session = helper->session;
            if (mayHaveEnded)
                session.wait(Clock::now() + kExitGrace);
            session.kill();

            LOGW << "Discarded " << kMetadataUtilName << " (pid " << session.pid() << ") "
                 << (operation == protocol::Operation::Read ? "reading " : "writing ") << io::toUtf8(file) << ": "
                 << reason << "; it " << endOf(session);
            throw Failure(reason);
        }

        // A request written to a helper whose response was not read yet. The lease holds the
        // helper out of the pool until the response is read and found good.
        struct HelperMetadataReader::Exchange
        {
            Lease lease;
            fs::path file;
            protocol::Operation operation;
            int64_t id;
        };

        protocol::Response HelperMetadataReader::serve(const fs::path &file, protocol::Operation operation,
                                                       const std::string &request, int64_t id)
        {
            Exchange exchange = send(acquire(), file, operation, request, id);
            return receive(exchange);
        }

        HelperMetadataReader::Exchange HelperMetadataReader::send(Lease lease, const fs::path &file,
                                                                  protocol::Operation operation,
                                                                  const std::string &request, int64_t id)
        {
            Exchange exchange{std::move(lease), file, operation, id};
            process::Session &session = exchange.lease.helper()->session;

            const auto written = session.writeLine(request, Clock::now() + options_.requestTimeout);
            requests_++;
            if (written == process::Session::WriteStatus::Closed)
                fail(exchange.lease, file, operation, std::string(kMetadataUtilName) + " is gone (its input is closed)",
                     true);
            if (written == process::Session::WriteStatus::Timeout)
                fail(exchange.lease, file, operation, std::string(kMetadataUtilName) + " did not read the request in time",
                     false);
            return exchange;
        }

        protocol::Response HelperMetadataReader::receive(Exchange &exchange)
        {
            using Status = process::Session::ReadStatus;
            Lease &lease = exchange.lease;
            const fs::path &file = exchange.file;
            const protocol::Operation operation = exchange.operation;
            const int64_t id = exchange.id;
            process::Session &session = lease.helper()->session;

            // The time the response may take starts when the caller waits for it, right after
            // the request for read() and write(). A caller that worked on the file meanwhile
            // (begin()) gave the helper that time on top: hashing a large video can take longer
            // than the limit, and a response that does not fit in the pipe is only complete once
            // the caller reads it, so that time says nothing of the helper.
            const auto deadline = Clock::now() + options_.requestTimeout;
            const auto read = readLineUnlessCanceled(session, deadline, options_.maxResponseBytes);
            if (!read)
            {
                // The helper is in the middle of a request: it cannot be reused
                lease.discard()->session.kill();
                throwCanceled();
            }
            const process::Session::ReadResult &line = *read;
            if (line.status == Status::Timeout)
                fail(lease, file, operation,
                     std::string(kMetadataUtilName) + " gave no response in " +
                         std::to_string(
                             std::chrono::duration_cast<std::chrono::seconds>(options_.requestTimeout).count()) +
                         " s",
                     false);
            if (line.status == Status::Eof)
                fail(lease, file, operation, std::string(kMetadataUtilName) + " closed its output without answering", true);
            if (line.status == Status::TooLong)
                fail(lease, file, operation,
                     std::string(kMetadataUtilName) + " sent a response longer than " +
                         std::to_string(options_.maxResponseBytes / 1024) + " KiB",
                     false);

            protocol::Response response;
            try
            {
                response = protocol::parseResponse(line.line, operation);
            }
            catch (const std::exception &e)
            {
                fail(lease, file, operation, std::string(kMetadataUtilName) + " sent an invalid response: " + e.what(),
                     false);
            }
            if (!response.id || *response.id != id)
                fail(lease, file, operation,
                     std::string(kMetadataUtilName) + " answered request " +
                         (response.id ? std::to_string(*response.id) : std::string("null")) + " instead of " +
                         std::to_string(id) + (response.ok ? std::string() : " (" + response.message + ")"),
                     false);
            if (!response.ok && !protocol::isFileError(response.error))
                fail(lease, file, operation,
                     std::string(kMetadataUtilName) + " rejected the request (" + response.error + ": " +
                         response.message + ")",
                     false);

            lease.keep();
            return response;
        }

        // One read: sent ahead by startIfFree() when a helper is free, otherwise in get(), which
        // waits its turn like read(). What making or sending the request threw before get() is
        // kept for get(), which throws what read() throws, in the same place.
        class HelperMetadataReader::PendingHelperRead final : public PendingRead::Request
        {
          public:
            PendingHelperRead(HelperMetadataReader &pool, const fs::path &file)
                : pool_(pool), file_(file), id_(pool_.nextId_++)
            {
                try
                {
                    request_ = protocol::readRequest(id_, file_);
                }
                catch (...)
                {
                    error_ = std::current_exception();
                }
            }

            // Sends the request now if that takes no wait for a helper. A caller that would
            // wait sends it in get() instead: waiting here would hold the caller, and the work
            // it does before get() (hashing a large file), behind reads it has nothing to do
            // with, and a helper it took while others wait would sit idle through that work.
            void startIfFree()
            {
                if (error_)
                    return;
                try
                {
                    if (std::optional<Lease> lease = pool_.tryAcquire())
                        exchange_.emplace(
                            pool_.send(std::move(*lease), file_, protocol::Operation::Read, request_, id_));
                }
                catch (...)
                {
                    error_ = std::current_exception();
                }
            }

            PendingHelperRead(const PendingHelperRead &) = delete;
            PendingHelperRead &operator=(const PendingHelperRead &) = delete;

            ~PendingHelperRead() override
            {
                if (!exchange_)
                    return;
                // The response was never read: the helper is killed rather than waited for (it
                // may hang) or reused (its next caller would read this response)
                try
                {
                    const std::unique_ptr<Helper> helper = exchange_->lease.discard();
                    helper->session.kill();
                    LOGD << "Discarded " << kMetadataUtilName << " (pid " << helper->session.pid() << ") reading "
                         << io::toUtf8(file_) << ": the read was abandoned before its response";
                }
                catch (...)
                {
                    // A destructor cannot report; the session kills the helper when it goes
                }
            }

            ReadResult get() override
            {
                try
                {
                    if (error_)
                        std::rethrow_exception(error_);
                    if (!exchange_)
                        // Not sent yet: the read takes its turn now
                        exchange_.emplace(
                            pool_.send(pool_.acquire(), file_, protocol::Operation::Read, request_, id_));
                    // Out of the pending state before the response is read: whatever receive()
                    // does, the helper has been dealt with (back in the pool, or discarded)
                    Exchange exchange = std::move(*exchange_);
                    exchange_.reset();

                    protocol::Response response = pool_.receive(exchange);
                    ReadResult result;
                    if (response.ok)
                        result.metadata = std::move(response.metadata);
                    else
                        result.message = response.message.empty() ? response.error : std::move(response.message);
                    return result;
                }
                catch (const Failure &failure)
                {
                    throw IndexException("Cannot read the metadata of " + io::toUtf8(file_) + ": " + failure.what());
                }
                catch (const protocol::ProtocolError &e)
                {
                    // The request itself cannot be made (the path cannot be made absolute): no
                    // helper involved
                    throw IndexException("Cannot read the metadata of " + io::toUtf8(file_) + ": " + e.what());
                }
            }

          private:
            HelperMetadataReader &pool_;
            const fs::path file_;
            const int64_t id_;
            std::string request_;
            // The request on its helper, until get() reads the response
            std::optional<Exchange> exchange_;
            std::exception_ptr error_;
        };

        PendingRead HelperMetadataReader::begin(const fs::path &file)
        {
            auto request = std::make_unique<PendingHelperRead>(*this, file);
            request->startIfFree();
            return PendingRead(std::move(request));
        }

        ReadResult HelperMetadataReader::read(const fs::path &file)
        {
            PendingHelperRead request(*this, file);
            return request.get();
        }

        WriteResult HelperMetadataReader::write(const fs::path &file, const TagAssignments &set)
        {
            const int64_t id = nextId_++;
            WriteResult result;
            try
            {
                protocol::Response response =
                    serve(file, protocol::Operation::Write, protocol::writeRequest(id, file, set), id);
                result.ok = response.ok;
                if (response.ok)
                    result.tags = std::move(response.tags);
                else
                    result.message = response.message.empty() ? response.error : std::move(response.message);
            }
            catch (const Failure &failure)
            {
                result.message = failure.what();
            }
            catch (const protocol::ProtocolError &e)
            {
                // The request itself cannot be sent (a key that is not text): no helper involved
                result.message = e.what();
            }
            return result;
        }

        // ---- what the tests observe

        size_t HelperMetadataReader::idleHelpers() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return idle_.size();
        }

        size_t HelperMetadataReader::busyHelpers() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return busy_;
        }

        size_t HelperMetadataReader::queuedCallers() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return queue_.size();
        }

        bool HelperMetadataReader::awaitWaiters(size_t count, std::chrono::milliseconds timeout) const
        {
            std::unique_lock<std::mutex> lock(mutex_);
            return changed_.wait_for(lock, timeout, [&] { return queue_.size() >= count; });
        }

        std::vector<unsigned long> HelperMetadataReader::idleHelperPids() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::vector<unsigned long> pids;
            for (const auto &helper : idle_)
                pids.push_back(helper->session.pid());
            return pids;
        }

    } // namespace exif
} // namespace ddb
