/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "untwine_runner.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "parallel.h"
#include "process_runner.h"

namespace ddb
{
    namespace untwine
    {

        namespace
        {

#ifdef _WIN32
            constexpr const char *kBinaryName = "untwine.exe";
            constexpr char kPathSep = ';';
#else
            constexpr const char *kBinaryName = "untwine";
            constexpr char kPathSep = ':';
#endif

            std::mutex g_cacheMutex;
            bool g_cacheValid = false;
            fs::path g_cachedBinary;

            bool isExecutableFile(const fs::path &p)
            {
                std::error_code ec;
                if (!fs::exists(p, ec) || ec) return false;
                if (!fs::is_regular_file(p, ec) || ec) return false;
#ifdef _WIN32
                return true;
#else
                // Check executable bit for current user/group/other
                auto perms = fs::status(p, ec).permissions();
                if (ec) return false;
                using fs::perms;
                return (perms & (perms::owner_exec | perms::group_exec | perms::others_exec)) != perms::none;
#endif
            }

            fs::path searchOnPath()
            {
                const char *path = std::getenv("PATH");
                if (!path || !*path) return {};

                std::string p(path);
                size_t start = 0;
                while (start <= p.size())
                {
                    size_t end = p.find(kPathSep, start);
                    if (end == std::string::npos) end = p.size();
                    std::string dir = p.substr(start, end - start);
                    if (!dir.empty())
                    {
                        fs::path candidate = fs::path(dir) / kBinaryName;
                        if (isExecutableFile(candidate)) return candidate;
                    }
                    if (end == p.size()) break;
                    start = end + 1;
                }
                return {};
            }

            // Tail of the captured output included in errorOut.
            constexpr size_t kErrorTailLength = 600;

            // Largest message accepted on the progress channel; anything bigger is
            // treated as a corrupt stream.
            constexpr uint32_t kMaxProgressMessage = 1u << 20;

            // Incremental decoder for Untwine's --progress_fd protocol:
            //   int32_t  msgId  - 1000=progress, 1001=error
            //   [1000] uint32_t percent | uint32_t ssize | char[ssize]
            //   [1001] uint32_t ssize   | char[ssize]
            // Messages are logged at debug level. An unknown msgId stops decoding
            // (the channel is still drained so the child never blocks on it).
            class ProgressDecoder
            {
              public:
                void feed(const char *data, size_t n)
                {
                    if (stopped_) return;
                    buf_.append(data, n);
                    size_t pos = 0;
                    while (!stopped_)
                    {
                        const size_t used = decodeOne(pos);
                        if (used == 0) break;
                        pos += used;
                    }
                    buf_.erase(0, pos);
                    if (stopped_) buf_.clear();
                }

              private:
                template <typename T>
                bool peek(size_t at, T &out) const
                {
                    if (buf_.size() < at + sizeof(T)) return false;
                    std::memcpy(&out, buf_.data() + at, sizeof(T));
                    return true;
                }

                // Decodes one complete message at pos; returns its size, or 0 when
                // more bytes are needed (or decoding stopped).
                size_t decodeOne(size_t pos)
                {
                    int32_t msgId = 0;
                    if (!peek(pos, msgId)) return 0;
                    if (msgId == 1000) // progress
                    {
                        uint32_t percent = 0, ssize = 0;
                        if (!peek(pos + 4, percent) || !peek(pos + 8, ssize)) return 0;
                        if (ssize > kMaxProgressMessage) return stop(msgId);
                        if (buf_.size() < pos + 12 + ssize) return 0;
                        LOGD << "[untwine] " << percent << "% " << buf_.substr(pos + 12, ssize);
                        return 12 + ssize;
                    }
                    if (msgId == 1001) // error
                    {
                        uint32_t ssize = 0;
                        if (!peek(pos + 4, ssize)) return 0;
                        if (ssize > kMaxProgressMessage) return stop(msgId);
                        if (buf_.size() < pos + 8 + ssize) return 0;
                        LOGD << "[untwine] ERROR: " << buf_.substr(pos + 8, ssize);
                        return 8 + ssize;
                    }
                    return stop(msgId);
                }

                size_t stop(int32_t msgId)
                {
                    LOGD << "[untwine] unexpected msgId=" << msgId << ", ignoring further progress messages";
                    stopped_ = true;
                    return 0;
                }

                std::string buf_;
                bool stopped_ = false;
            };

        } // namespace

        fs::path findUntwineBinary(bool forceRefresh)
        {
            std::lock_guard<std::mutex> lock(g_cacheMutex);
            if (!forceRefresh && g_cacheValid) return g_cachedBinary;

            fs::path found;

            // 1. Explicit override via env var. When DDB_UNTWINE_PATH is set
            // (non-empty), it is authoritative: either it points to a usable
            // binary or no binary is considered available (so callers can pin
            // the backend deterministically and tests can simulate "missing").
            if (const char *override = std::getenv("DDB_UNTWINE_PATH"))
            {
                if (override[0] != '\0')
                {
                    fs::path p(override);
                    if (isExecutableFile(p)) found = p;
                    g_cachedBinary = found;
                    g_cacheValid = true;
                    if (!found.empty())
                        LOGD << "Untwine binary discovered via DDB_UNTWINE_PATH at " << found.string();
                    else
                        LOGD << "DDB_UNTWINE_PATH=" << override << " is not an executable file; treating Untwine as unavailable";
                    return found;
                }
            }

            // 2. Folder of the current executable (where untwine is bundled by build/packaging)
            {
                fs::path exeDir = io::getExeFolderPath();
                if (!exeDir.empty())
                {
                    fs::path candidate = exeDir / kBinaryName;
                    if (isExecutableFile(candidate)) found = candidate;
                }
            }

            // 3. <install_prefix>/bin (parent of exe folder + /bin) - covers cmake --install layouts
            if (found.empty())
            {
                fs::path exeDir = io::getExeFolderPath();
                if (!exeDir.empty() && exeDir.has_parent_path())
                {
                    fs::path candidate = exeDir.parent_path() / "bin" / kBinaryName;
                    if (isExecutableFile(candidate)) found = candidate;
                }
            }

            // 4. System PATH
            if (found.empty()) found = searchOnPath();

            g_cachedBinary = found;
            g_cacheValid = true;

            if (!found.empty())
                LOGD << "Untwine binary discovered at " << found.string();
            else
                LOGD << "Untwine binary not found in DDB_UNTWINE_PATH, exe folder, install prefix or PATH";

            return found;
        }

        CopcBackend resolveBackend(CopcBackend requested)
        {
            // Hard override: legacy switch
            if (const char *forcePdal = std::getenv("DDB_USE_PDAL_COPC"))
            {
                if (forcePdal[0] == '1')
                {
                    LOGD << "DDB_USE_PDAL_COPC=1 forces PDAL COPC backend";
                    return CopcBackend::Pdal;
                }
            }

            // Explicit backend env (overrides argument)
            if (const char *envBackend = std::getenv("DDB_COPC_BACKEND"))
            {
                std::string s(envBackend);
                std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
                if (s == "untwine") requested = CopcBackend::Untwine;
                else if (s == "pdal") requested = CopcBackend::Pdal;
                else if (s == "auto") requested = CopcBackend::Auto;
                // unknown values are silently ignored
            }

            if (requested == CopcBackend::Pdal) return CopcBackend::Pdal;

            fs::path bin = findUntwineBinary();

            if (requested == CopcBackend::Untwine)
            {
                if (bin.empty())
                    throw UntwineException("Untwine backend requested but no untwine binary could be located. "
                                           "Set DDB_UNTWINE_PATH or place untwine next to the DroneDB executable.");
                return CopcBackend::Untwine;
            }

            // Auto
            return bin.empty() ? CopcBackend::Pdal : CopcBackend::Untwine;
        }

        bool runUntwine(const std::vector<std::string> &inputFiles,
                        const fs::path &outputFile,
                        const fs::path &tempDir,
                        std::string &errorOut,
                        const fs::path &binary)
        {
            const auto result = runUntwineResult(inputFiles, outputFile, tempDir, binary);
            errorOut = result.error;
            return result.succeeded;
        }

        ToolRunResult runUntwineResult(const std::vector<std::string> &inputFiles,
                                      const fs::path &outputFile,
                                      const fs::path &tempDir,
                                      const fs::path &binary,
                                      const process::Options &options)
        {
            ToolRunResult result;
            fs::path bin = binary.empty() ? findUntwineBinary() : binary;
            if (bin.empty() || !isExecutableFile(bin))
            {
                result.error = "untwine binary not found";
                return result;
            }
            if (inputFiles.empty())
            {
                result.error = "no input files provided";
                return result;
            }

            std::string filesArg;
            for (size_t i = 0; i < inputFiles.size(); ++i)
            {
                if (i > 0) filesArg.push_back(',');
                filesArg.append(inputFiles[i]);
            }

            std::error_code ec;
            auto failIo = [&result](const std::string &operation, const std::error_code &code) {
                result.errorCode = code;
                result.error = operation + ": " + code.message();
                return result;
            };
            if (!outputFile.parent_path().empty()) {
                fs::create_directories(outputFile.parent_path(), ec);
                if (ec) return failIo("Cannot create COPC output directory", ec);
            }
            fs::remove(outputFile, ec);
            if (ec) return failIo("Cannot remove stale COPC output", ec);

            std::vector<std::string> args = {"--files", filesArg, "--output_dir", outputFile.string()};
            if (!tempDir.empty())
            {
                fs::create_directories(tempDir, ec);
                if (ec) return failIo("Cannot create Untwine temporary directory", ec);
                args.push_back("--temp_dir");
                args.push_back(tempDir.string());
            }
            args.push_back("--progress_fd");
            args.push_back(process::kExtraChannelPlaceholder);

            LOGD << "Spawning untwine (" << inputFiles.size() << " input file(s)): "
                 << process::formatCommandLine(bin, args);
            // untwine has no thread option: it runs up to 10 threads of its own (8 file readers,
            // then a pool of 10). They are counted as CPU tokens while it runs (DQ-15), so the
            // other parallel sections of the process take fewer.
            constexpr size_t UntwineThreads = 10;
            const auto cpu = parallel::tryAcquireThreads(UntwineThreads);
            LOGD << "untwine CPU tokens: " << cpu.count();

            ProgressDecoder progress;
            process::Options popts = options;
            popts.extraChannel = true;
            popts.onExtraData = [&progress](const char *data, size_t n) { progress.feed(data, n); };
            popts.onOutputLine = [](const std::string &line) { LOGD << "[untwine] " << line; };

            const process::Result res = process::run(bin, args, popts);
            if (res.descendantsTerminated)
                LOGW << "untwine " << res.describe();
            if (!res.success())
            {
                result.timedOut = res.timedOut;
                std::ostringstream oss;
                oss << "untwine " << res.describe();
                if (!res.outputTail.empty()) oss << ": " << res.tail(kErrorTailLength);
                result.error = oss.str();
                return result;
            }
            if (!fs::exists(outputFile, ec) || ec)
            {
                result.errorCode = ec;
                result.error = "untwine reported success but output file is missing: " + outputFile.string();
                return result;
            }
            result.succeeded = true;
            return result;
        }

    } // namespace untwine
} // namespace ddb
