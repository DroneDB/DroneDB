/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "raster_helper.h"

#include <algorithm>
#include <csignal>
#include <cstring>
#include <fstream>
#include <system_error>
#include <utility>

#include "cancellation.h"
#include "exif.h"
#include "http_client.h"
#include "logger.h"
#include "mio.h"
#include "raster_artefacts.h"
#include "stringutil.h"
#include "thermal.h"
#include "tool_locator.h"
#include "tool_run_support.h"

namespace ddb
{
    namespace raster
    {

        namespace
        {
            using Clock = std::chrono::steady_clock;

            // What of the output of `gdal-util --version` goes to an error message
            constexpr size_t kVersionOutputInMessage = 300;

            // The environment every helper starts with: a bounded heap for each worker, and no
            // grid downloaded from the network by PROJ
            const std::vector<std::pair<std::string, std::string>> &helperEnvironment()
            {
                static const std::vector<std::pair<std::string, std::string>> environment = {
                    {"MALLOC_ARENA_MAX", "2"}, {"PROJ_NETWORK", "OFF"}};
                return environment;
            }

            std::string missingProgramMessage()
            {
                const std::string hint = std::string("set ") + kGdalUtilEnv + " to the " + kGdalUtilName +
                                         " executable, or install it next to ddb or on the PATH";
                if (const auto configured = io::getEnvUtf8(kGdalUtilEnv); configured && !configured->empty())
                    return std::string(kGdalUtilEnv) + "=" + *configured + " is not an executable file, so " +
                           kGdalUtilName + " cannot be used to read raster and vector files; " + hint;
                return std::string(kGdalUtilName) + " was not found, and it is needed to read raster and vector files; " +
                       hint;
            }

            fs::path locateByTool()
            {
                fs::path program = tools::findTool(kGdalUtilName, kGdalUtilEnv);
                // The answer is cached; a program installed since is worth one more look
                if (program.empty())
                    program = tools::findTool(kGdalUtilName, kGdalUtilEnv, true);
                return program;
            }

            // The accept rule: the protocol major this client implements, at least its minor
            helper::VersionVerdict acceptVersion(const std::string &output)
            {
                const auto version = parseVersionOutput(output);
                if (!version)
                    return {false, "its --version printed no protocol version (" +
                                       helper::lastBytes(output, kVersionOutputInMessage) + ")"};
                if (!raster::isAccepted(*version))
                    return {false, "it speaks protocol " + raster::toString(*version) + ", this ddb needs " +
                                       raster::toString({kMajorVersion, kMinorVersion}) + " or a later " +
                                       std::to_string(kMajorVersion) + ".x; set " + kGdalUtilEnv + " to a compatible " +
                                       kGdalUtilName};
                return {true, "protocol " + raster::toString(*version)};
            }

            helper::PoolOptions poolOptions(const GdalUtilHelperOptions &options)
            {
                helper::PoolOptions pool;
                pool.name = kGdalUtilName;
                pool.locate = options.locate ? options.locate : std::function<fs::path()>(locateByTool);
                pool.missingMessage = missingProgramMessage;
                pool.validateVersion = acceptVersion;
                pool.session.envOverrides = helperEnvironment();
                pool.limit = options.limit;
                pool.now = options.now;
                pool.idleTimeout = options.idleTimeout;
                pool.versionTimeout = options.versionTimeout;
                pool.closeTimeout = options.closeTimeout;
                pool.maxResponseBytes = options.maxResponseBytes;
                return pool;
            }

            fs::path absolutePath(const fs::path &path)
            {
                return path.empty() ? path : fs::absolute(path).lexically_normal();
            }

            // ---- failures

            // The log line of a failure of the helper: the operation, the error code and the file
            // (the pool logs the end of the helper's stderr when it has to kill the helper)
            void logFailure(const char *op, const fs::path &path, ErrorCode code, const std::string &message)
            {
                const std::string text = std::string(kGdalUtilName) + " " + op + " of " + io::toUtf8(path) +
                                         " failed (" + toString(code) + "): " + message;
                switch (code)
                {
                case ErrorCode::Crashed:
                    LOGE << text;
                    break;
                case ErrorCode::Unreadable:
                case ErrorCode::Policy:
                    LOGD << text;
                    break;
                default:
                    LOGW << text;
                    break;
                }
            }

            // Throws what the failure of an operation means to the host (IRasterHelper)
            [[noreturn]] void raiseFailure(const char *op, const fs::path &path, ErrorCode code,
                                           const std::string &message)
            {
                logFailure(op, path, code, message);
                switch (code)
                {
                case ErrorCode::Limit:
                case ErrorCode::Crashed:
                case ErrorCode::Timeout:
                    throw RasterNotReadException(code, message);
                case ErrorCode::NoSpace:
                    throw FSException(message, std::make_error_code(std::errc::no_space_on_device));
                case ErrorCode::Unreadable:
                case ErrorCode::Policy:
                case ErrorCode::Internal:
                    break;
                }
                throw RasterOpException(code, message);
            }

            // A one-shot process that used up the cpu time or the file size its sandbox allows is
            // ended by the system with one of these signals and writes no response, where a
            // worker of the server is told by the server (executor.cpp)
            bool limitSignal(int signal)
            {
#ifdef SIGXCPU
                if (signal == SIGXCPU)
                    return true;
#endif
#ifdef SIGXFSZ
                if (signal == SIGXFSZ)
                    return true;
#endif
                (void)signal;
                return false;
            }

            [[noreturn]] void raiseFailure(const char *op, const fs::path &path, const Response &response)
            {
                raiseFailure(op, path, response.error,
                             response.message.empty() ? std::string(toString(response.error)) : response.message);
            }
        } // namespace

        std::string notReadWarning(const std::string &reason)
        {
            return "raster not read: " + reason;
        }

        // ---- GdalUtilHelper

        GdalUtilHelper::GdalUtilHelper(Options options) : options_(std::move(options)), pool_(poolOptions(options_))
        {
        }

        GdalUtilHelper::~GdalUtilHelper() = default;

        namespace
        {
            std::atomic<GdalUtilHelper *> g_instance{nullptr};
        } // namespace

        GdalUtilHelper &GdalUtilHelper::instance()
        {
            // Never destroyed (see the header)
            static GdalUtilHelper *const helper = [] {
                auto *created = new GdalUtilHelper();
                g_instance.store(created, std::memory_order_release);
                return created;
            }();
            return *helper;
        }

        size_t GdalUtilHelper::shutdownInstance()
        {
            GdalUtilHelper *helper = g_instance.load(std::memory_order_acquire);
            return helper != nullptr ? helper->shutdown() : 0;
        }

        // One request on a helper of the pool. The response is accepted by `accept` while the helper
        // is still held: a result the host cannot understand makes the helper a broken one
        // (killed and dropped), like a line that is not JSON.
        Response GdalUtilHelper::serve(const char *op, const fs::path &path, const fs::path &out, json params,
                                       std::chrono::milliseconds timeout, const ResponseCheck &accept)
        {
            // A caller whose work is canceled starts nothing: a helper may well answer before the pool
            // looks at the cancellation
            cancel::throwIfCanceled();

            Request request;
            request.id = nextId_++;
            request.op = op;
            request.path = absolutePath(path);
            request.out = absolutePath(out);
            request.params = std::move(params);
            const std::string line = encodeRequest(request);

            helper::Pool::Lease lease = pool_.acquire();
            lease.setContext(std::string(op) + " of " + io::toUtf8(path));
            const auto deadline = Clock::now() + timeout;
            try
            {
                const std::string text = lease.exchange(line, deadline);

                Response response;
                try
                {
                    response = decodeResponse(text);
                }
                catch (const ProtocolError &e)
                {
                    lease.fail(std::string(kGdalUtilName) + " sent an invalid response: " + e.what(), false);
                }
                if (response.id != std::optional<int64_t>(request.id))
                    lease.fail(std::string(kGdalUtilName) + " answered request " +
                                   (response.id ? std::to_string(*response.id) : std::string("null")) + " instead of " +
                                   std::to_string(request.id),
                               false);
                if (response.ok && accept)
                {
                    try
                    {
                        accept(response);
                    }
                    catch (const ProtocolError &e)
                    {
                        lease.fail(std::string(kGdalUtilName) + " sent an invalid result: " + e.what(), false);
                    }
                }
                lease.keep();
                return response;
            }
            catch (const helper::Failure &failure)
            {
                // The helper is dropped and logged by the pool; the host says what it means. The
                // deadline tells a helper that did not answer in time from one that broke.
                raiseFailure(op, path, Clock::now() >= deadline ? ErrorCode::Timeout : ErrorCode::Crashed,
                             failure.what());
            }
        }

        ProbeResult GdalUtilHelper::probe(const fs::path &path, const ProbeParams &params)
        {
            ProbeResult result;
            const Response response = serve(
                op::kProbe, path, fs::path(), toJson(params), options_.probeTimeout,
                [&result](const Response &ok) { result = probeResultFromJson(ok.result); });
            if (!response.ok)
                raiseFailure(op::kProbe, path, response);
            return result;
        }

        void GdalUtilHelper::thumbnail(const fs::path &path, const fs::path &out, const ThumbnailParams &params)
        {
            const Response response =
                serve(op::kThumbnail, path, out, toJson(params), options_.thumbnailTimeout, ResponseCheck());
            if (!response.ok)
                raiseFailure(op::kThumbnail, path, response);
        }

        void GdalUtilHelper::normalize(const fs::path &path, const fs::path &out, const json &params)
        {
            const Response response = serve(op::kNormalize, path, out, params, options_.conversionTimeout, ResponseCheck());
            if (!response.ok)
                raiseFailure(op::kNormalize, path, response);
        }

        void GdalUtilHelper::geoproject(const fs::path &path, const fs::path &out, const json &params)
        {
            const Response response = serve(op::kGeoproject, path, out, params, options_.conversionTimeout, ResponseCheck());
            if (!response.ok)
                raiseFailure(op::kGeoproject, path, response);
        }

        void GdalUtilHelper::buildCog(const fs::path &path, const fs::path &out, const json &params,
                                      const ProgressCallback &progress)
        {
            runOneShot(op::kCog, path, out, params, progress);
        }

        void GdalUtilHelper::buildVector(const fs::path &path, const fs::path &out, const json &params,
                                         const ProgressCallback &progress)
        {
            runOneShot(op::kVector, path, out, params, progress);
        }

        // ---- one-shot requests

        namespace
        {
            // What the lines of a `--request` process say: progress, and the response. Anything
            // else is a message of a library, which the process runner keeps in its output tail.
            class OneShotOutput
            {
              public:
                explicit OneShotOutput(const ProgressCallback &progress) : progress_(progress) {}

                void onLine(const std::string &line)
                {
                    if (const auto fraction = decodeProgress(line))
                    {
                        if (progress_)
                            progress_(*fraction);
                        return;
                    }
                    if (line.empty() || line.front() != '{')
                        return;
                    const json parsed = json::parse(line, nullptr, false);
                    if (parsed.is_object() && parsed.contains("ok"))
                    {
                        try
                        {
                            response_ = decodeResponse(line);
                        }
                        catch (const ProtocolError &e)
                        {
                            LOGW << kGdalUtilName << " wrote a response that is not valid: " << e.what();
                        }
                    }
                }

                const std::optional<Response> &response() const { return response_; }

              private:
                const ProgressCallback &progress_;
                std::optional<Response> response_;
            };
        } // namespace

        void GdalUtilHelper::runOneShot(const char *op, const fs::path &path, const fs::path &out, json params,
                                        const ProgressCallback &progress)
        {
            const process::IProcessRunner &runner =
                options_.runner != nullptr ? *options_.runner : process::systemProcessRunner();

            cancel::throwIfCanceled();

            const fs::path program = options_.locate ? options_.locate() : locateByTool();
            if (program.empty())
                throw HelperUnavailableException(missingProgramMessage());

            // The version check the pool makes for its helpers: another protocol major is a
            // program this client does not understand
            process::Options versionRun;
            versionRun.envOverrides = helperEnvironment();
            versionRun.idleTimeout = options_.versionTimeout;
            versionRun.maxRuntime = options_.versionTimeout;
            const process::Result version = runner.run(program, {"--version"}, versionRun);
            if (!version.started)
                throw HelperUnavailableException("Cannot use " + std::string(kGdalUtilName) + " at " +
                                                 io::toUtf8(program) + ": " + version.describe());
            const helper::VersionVerdict verdict = acceptVersion(version.outputTail);
            if (!verdict.accepted)
                throw HelperUnavailableException("Cannot use " + std::string(kGdalUtilName) + " at " +
                                                 io::toUtf8(program) + ": " + verdict.text);

            Request request;
            request.id = nextId_++;
            request.op = op;
            request.path = absolutePath(path);
            request.out = absolutePath(out);
            request.params = std::move(params);

            OneShotOutput output(progress);
            process::Options run;
            run.envOverrides = helperEnvironment();
            run.maxRuntime = tools::toolMaxRuntime(kGdalUtilMaxSecondsEnv, kGdalUtilDefaultMaxRuntime);
            run.onOutputLine = [&output](const std::string &line) { output.onLine(line); };
            const process::Result result = runner.run(program, {"--request", encodeRequest(request)}, run);

            if (output.response() && output.response()->id == std::optional<int64_t>(request.id))
            {
                if (!output.response()->ok)
                {
                    // A build that runs out of space is retried when there is some again, as
                    // every build reports it
                    if (output.response()->error == ErrorCode::NoSpace)
                    {
                        logFailure(op, path, ErrorCode::NoSpace, output.response()->message);
                        throw RetryableBuildException(output.response()->message, RetryableBuildReason::NoSpaceOnDevice);
                    }
                    raiseFailure(op, path, *output.response());
                }
                return;
            }

            // No response: the process did not run, was killed, or ended on its own
            if (!result.started)
                throw HelperUnavailableException("Cannot run " + std::string(kGdalUtilName) + " at " +
                                                 io::toUtf8(program) + ": " + result.describe());
            std::string text = result.failureText(300);
            if (text.empty())
                text = std::string(kGdalUtilName) + " ended without a response";
            if (result.timedOut)
            {
                // A process that printed nothing for the idle time hung; one that ran for its whole
                // maximum is a build that did not finish
                logFailure(op, path, ErrorCode::Timeout, text);
                if (run.maxRuntime.count() > 0 && result.duration >= run.maxRuntime)
                    throw RasterNotReadException(ErrorCode::Timeout, text);
                throw RetryableBuildException(text, RetryableBuildReason::IdleTimeout);
            }
            raiseFailure(op, path, limitSignal(result.termSignal) ? ErrorCode::Limit : ErrorCode::Crashed, text);
        }

        // ---- the helper of the process

        namespace
        {
            // The helper a ScopedRasterHelper installed, nullptr for the default
            std::atomic<IRasterHelper *> g_override{nullptr};
        } // namespace

        IRasterHelper &rasterHelper()
        {
            if (IRasterHelper *helper = g_override.load(std::memory_order_acquire))
                return *helper;
            return GdalUtilHelper::instance();
        }

        ScopedRasterHelper::ScopedRasterHelper(IRasterHelper &helper)
            : previous_(g_override.exchange(&helper, std::memory_order_acq_rel))
        {
        }

        ScopedRasterHelper::~ScopedRasterHelper()
        {
            g_override.store(previous_, std::memory_order_release);
        }

        size_t closeRasterHelpers()
        {
            return GdalUtilHelper::shutdownInstance();
        }

        // ---- probing

        const ProbeOutcome &probeFile(FingerprintContext &ctx, const fs::path &path, const ProbeParams &params)
        {
            if (ctx.probe && ctx.probe->kind == params.kind && ctx.probe->path == path)
                return *ctx.probe;

            auto outcome = std::make_shared<ProbeOutcome>();
            outcome->kind = params.kind;
            outcome->path = path;
            try
            {
                outcome->result = rasterHelper().probe(path, params);
            }
            catch (const RasterNotReadException &e)
            {
                outcome->notRead = e.what();
            }
            catch (const RasterOpException &e)
            {
                // GDAL cannot open it, or the policy refuses it: not a failure of the helper
                if (e.code() == ErrorCode::Internal)
                    throw;
                LOGD << "Cannot read " << io::toUtf8(path) << ": " << e.what();
            }
            ctx.probe = std::move(outcome);
            return *ctx.probe;
        }

        // ---- thumbnails

        bool looksLikeThumbnail(const std::string &bytes)
        {
            // "RIFF", the size of the rest of the file (little endian), "WEBP"
            if (bytes.size() < 12 || bytes.size() > kMaxThumbnailBytes)
                return false;
            if (std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WEBP", 4) != 0)
                return false;
            uint64_t declared = 0;
            for (int i = 3; i >= 0; i--)
                declared = (declared << 8) | static_cast<unsigned char>(bytes[4 + i]);
            return declared + 8 == bytes.size();
        }

        namespace
        {
            // The sink of a download: the bytes go to the file until there would be more than
            // `maxBytes`, when it fails (and with it the transfer) and says why
            class CappedFileSink : public std::streambuf
            {
              public:
                CappedFileSink(const fs::path &target, uint64_t maxBytes)
                    : file_(target, std::ios::binary), maxBytes_(maxBytes)
                {
                }

                bool exceeded() const { return exceeded_; }
                // Whether every byte reached the file, once it is closed
                bool close()
                {
                    file_.close();
                    return !file_.fail();
                }

              protected:
                std::streamsize xsputn(const char *data, std::streamsize count) override
                {
                    if (count < 0 || static_cast<uint64_t>(count) > maxBytes_ - written_)
                    {
                        exceeded_ = true;
                        return 0;
                    }
                    file_.write(data, count);
                    if (!file_)
                        return 0;
                    written_ += static_cast<uint64_t>(count);
                    return count;
                }
                int_type overflow(int_type c) override
                {
                    if (traits_type::eq_int_type(c, traits_type::eof()))
                        return traits_type::not_eof(c);
                    const char ch = traits_type::to_char_type(c);
                    return xsputn(&ch, 1) == 1 ? c : traits_type::eof();
                }

              private:
                std::ofstream file_;
                uint64_t maxBytes_;
                uint64_t written_ = 0;
                bool exceeded_ = false;
            };
        } // namespace

        void downloadInput(const std::string &url, const fs::path &target, uint64_t maxBytes)
        {
            bool written = false;
            bool announcedTooLarge = false;
            bool exceeded = false;
            HttpResponse response;
            {
                CappedFileSink buffer(target, maxBytes);
                std::ostream sink(&buffer);
                HttpRequest request;
                request.url = url;
                // The length the server announces stops the transfer before the body comes
                request.progress = [maxBytes, &announcedTooLarge](const HttpProgress &progress) {
                    announcedTooLarge = progress.downloadTotal > maxBytes;
                    return !announcedTooLarge;
                };
                response = defaultHttpClient().download(request, sink);
                exceeded = announcedTooLarge || buffer.exceeded();
                // A disk-full or I/O failure stops the transfer, and also shows on the stream state
                written = buffer.close() && !sink.fail();
            }
            if (exceeded)
            {
                io::assureIsRemoved(target);
                throw NetException("Cannot download " + url + ": it is larger than the " + std::to_string(maxBytes) +
                                   " bytes a remote file may have");
            }
            if (!written || response.hasTransportError() || response.status < 200 || response.status >= 300)
            {
                io::assureIsRemoved(target);
                throw NetException("Cannot download " + url + " (HTTP " + std::to_string(response.status) + "): " +
                                   (written ? response.transportMessage : std::string("write error")));
            }
        }

        std::string urlFileExtension(const std::string &url)
        {
            std::string_view path(url);
            path = path.substr(0, path.find_first_of("?#"));
            if (const auto scheme = path.find("://"); scheme != std::string_view::npos)
            {
                // The authority ("host:port") is no part of the path
                const auto pathStart = path.find('/', scheme + 3);
                path = pathStart == std::string_view::npos ? std::string_view() : path.substr(pathStart);
            }
            const std::string_view name = path.substr(path.find_last_of('/') + 1);
            const auto dot = name.find_last_of('.');
            if (dot == std::string_view::npos || dot == 0)
                return "";
            const std::string_view extension = name.substr(dot + 1);
            if (extension.empty() || extension.size() > 16 ||
                !std::all_of(extension.begin(), extension.end(), [](char c) { return str::isAlnumChar(c); }))
                return "";
            return "." + std::string(extension);
        }

        namespace
        {
            // The content of the file the helper wrote: a regular file (not a link to another one)
            // of a size a thumbnail can have
            std::string readHelperOutput(const fs::path &file)
            {
                std::error_code ec;
                const auto status = fs::symlink_status(file, ec);
                if (ec || status.type() != fs::file_type::regular)
                    throw GDALException(std::string(kGdalUtilName) + " wrote no thumbnail file");
                const auto size = fs::file_size(file, ec);
                if (ec || size == 0 || size > kMaxThumbnailBytes)
                    throw GDALException(std::string(kGdalUtilName) + " wrote a thumbnail of " +
                                        (ec ? std::string("unknown size") : std::to_string(size) + " bytes") +
                                        ", which is not one");
                std::ifstream in(file, std::ios::binary);
                std::string bytes(static_cast<size_t>(size), '\0');
                in.read(bytes.data(), static_cast<std::streamsize>(size));
                if (in.gcount() != static_cast<std::streamsize>(size))
                    throw GDALException(std::string("Cannot read the thumbnail ") + io::toUtf8(file));
                return bytes;
            }
        } // namespace

        std::string renderThumbnailWebp(const fs::path &path, const ThumbnailParams &params)
        {
            if (params.size <= 0)
                throw InvalidArgsException("thumbSize must be greater than 0");

            // In the temp folder of the system, not in the thumbnail cache: the destination of a
            // thumbnail is whatever the caller names (and the memory API has none), and the copy
            // into it is made by the caller as any thumbnail is written (atomically)
            const io::ScopedTempDir request = raster::makePrivateFolder(fs::temp_directory_path(), "ddb-thumb-");

            fs::path input = path;
            const std::string utf8 = io::toUtf8(path);
            if (io::isNetworkPath(utf8))
            {
                // The helper reads files, never the network
                input = request.path() / io::fromUtf8("input" + urlFileExtension(utf8));
                downloadInput(utf8, input);
            }

            // The helper reads no tags: what they say of the file goes in the request
            ThumbnailParams sent = params;
            if (needsThermalTags(sent))
                sent.thermalTags = hasThermalTags(io::toUtf8(input));

            const io::ScopedTempDir outputFolder = raster::makePrivateFolder(request.path(), "out-");
            const fs::path output = outputFolder.path() / "thumbnail.webp";
            rasterHelper().thumbnail(input, output, sent);

            std::string bytes = readHelperOutput(output);
            if (!looksLikeThumbnail(bytes))
            {
                LOGW << kGdalUtilName << " thumbnail of " << utf8 << " is not a WebP file; it is dropped";
                throw GDALException(std::string(kGdalUtilName) + " wrote a file that is not a thumbnail");
            }
            return bytes;
        }

    } // namespace raster
} // namespace ddb
