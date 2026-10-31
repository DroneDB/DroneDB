/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "exif_helper_pool.h"

#include <exception>
#include <optional>
#include <utility>

#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "tool_locator.h"

namespace ddb
{
    namespace exif
    {

        namespace
        {
            using Clock = std::chrono::steady_clock;

            // What of the output of `metadata-util --version` goes to an error message
            constexpr size_t kVersionOutputInMessage = 300;

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

            // The accept rule: the protocol major this client implements, at least its minor
            helper::VersionVerdict acceptVersion(const std::string &output)
            {
                const auto version = protocol::parseVersionOutput(output);
                if (!version)
                    return {false, "its --version printed no protocol version (" +
                                       helper::lastBytes(output, kVersionOutputInMessage) + ")"};
                if (!protocol::isAccepted(*version))
                    return {false, "it speaks protocol " + protocol::toString(*version) + ", this ddb needs " +
                                       protocol::toString({protocol::kMajorVersion, protocol::kMinorVersion}) +
                                       " or a later " + std::to_string(protocol::kMajorVersion) + ".x; set " +
                                       kMetadataUtilEnv + " to a compatible " + kMetadataUtilName};
                return {true, "protocol " + protocol::toString(*version)};
            }

            helper::PoolOptions poolOptions(const HelperMetadataReaderOptions &options)
            {
                helper::PoolOptions pool;
                pool.name = kMetadataUtilName;
                pool.locate = options.locate ? options.locate : std::function<fs::path()>(locateByTool);
                pool.missingMessage = missingProgramMessage;
                pool.validateVersion = acceptVersion;
                pool.raiseUnavailable = [](const std::string &message) { throw MetadataHelperException(message); };
                pool.limit = options.limit;
                pool.now = options.now;
                pool.idleTimeout = options.idleTimeout;
                pool.versionTimeout = options.versionTimeout;
                pool.closeTimeout = options.closeTimeout;
                pool.maxResponseBytes = options.maxResponseBytes;
                return pool;
            }
        } // namespace

        HelperMetadataReader::HelperMetadataReader(Options options)
            : requestTimeout_(options.requestTimeout), pool_(poolOptions(options))
        {
        }

        HelperMetadataReader::~HelperMetadataReader() = default;

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

        // ---- requests

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
            Exchange exchange = send(pool_.acquire(), file, operation, request, id);
            return receive(exchange);
        }

        HelperMetadataReader::Exchange HelperMetadataReader::send(Lease lease, const fs::path &file,
                                                                  protocol::Operation operation,
                                                                  const std::string &request, int64_t id)
        {
            Exchange exchange{std::move(lease), file, operation, id};
            exchange.lease.setContext(std::string(operation == protocol::Operation::Read ? "reading " : "writing ") +
                                      io::toUtf8(file));
            exchange.lease.send(request, Clock::now() + requestTimeout_);
            return exchange;
        }

        protocol::Response HelperMetadataReader::receive(Exchange &exchange)
        {
            Lease &lease = exchange.lease;
            const protocol::Operation operation = exchange.operation;
            const int64_t id = exchange.id;
            const std::string name = kMetadataUtilName;

            // The time the response may take starts when the caller waits for it, right after
            // the request for read() and write(). A caller that worked on the file meanwhile
            // (begin()) gave the helper that time on top: hashing a large video can take longer
            // than the limit, and a response that does not fit in the pipe is only complete once
            // the caller reads it, so that time says nothing of the helper.
            const std::string line = lease.receive(Clock::now() + requestTimeout_);

            protocol::Response response;
            try
            {
                response = protocol::parseResponse(line, operation);
            }
            catch (const std::exception &e)
            {
                lease.fail(name + " sent an invalid response: " + e.what(), false);
            }
            if (!response.id || *response.id != id)
                lease.fail(name + " answered request " +
                               (response.id ? std::to_string(*response.id) : std::string("null")) + " instead of " +
                               std::to_string(id) + (response.ok ? std::string() : " (" + response.message + ")"),
                           false);
            if (!response.ok && !protocol::isFileError(response.error))
                lease.fail(name + " rejected the request (" + response.error + ": " + response.message + ")", false);

            lease.keep();
            return response;
        }

        // One read: sent ahead by startIfFree() when a helper is free, otherwise in get(), which
        // waits its turn like read(). What making or sending the request threw before get() is
        // kept for get(), which throws what read() throws, in the same place.
        class HelperMetadataReader::PendingHelperRead final : public PendingRead::Request
        {
          public:
            PendingHelperRead(HelperMetadataReader &reader, const fs::path &file)
                : reader_(reader), file_(file), id_(reader_.nextId_++)
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
                    if (std::optional<Lease> lease = reader_.pool_.tryAcquire())
                        exchange_.emplace(
                            reader_.send(std::move(*lease), file_, protocol::Operation::Read, request_, id_));
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
                exchange_->lease.abandon("the read was abandoned before its response");
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
                            reader_.send(reader_.pool_.acquire(), file_, protocol::Operation::Read, request_, id_));
                    // Out of the pending state before the response is read: whatever receive()
                    // does, the helper has been dealt with (back in the pool, or discarded)
                    Exchange exchange = std::move(*exchange_);
                    exchange_.reset();

                    protocol::Response response = reader_.receive(exchange);
                    ReadResult result;
                    if (response.ok)
                        result.metadata = std::move(response.metadata);
                    else
                        result.message = response.message.empty() ? response.error : std::move(response.message);
                    return result;
                }
                catch (const helper::Failure &failure)
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
            HelperMetadataReader &reader_;
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
            catch (const helper::Failure &failure)
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

    } // namespace exif
} // namespace ddb
