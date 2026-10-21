/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "buildlod_runner.h"

#include <cstdlib>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "logger.h"
#include "mio.h"
#include "process_runner.h"

namespace ddb
{
    namespace buildlod
    {

        namespace
        {

#ifdef _WIN32
            constexpr const char *kBinaryName = "build-lod.exe";
            constexpr char kPathSep = ';';
#else
            constexpr const char *kBinaryName = "build-lod";
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
                auto perms = fs::status(p, ec).permissions();
                if (ec) return false;
                using fs::perms;
                return (perms & (perms::owner_exec | perms::group_exec | perms::others_exec)) != perms::none;
#endif
            }

            fs::path searchOnPath()
            {
                const auto path = io::getEnvUtf8("PATH");
                if (!path || path->empty()) return {};

                const std::string &p = *path;
                size_t start = 0;
                while (start <= p.size())
                {
                    size_t end = p.find(kPathSep, start);
                    if (end == std::string::npos) end = p.size();
                    std::string dir = p.substr(start, end - start);
                    if (!dir.empty())
                    {
                        fs::path candidate = io::fromUtf8(dir) / kBinaryName;
                        if (isExecutableFile(candidate)) return candidate;
                    }
                    if (end == p.size()) break;
                    start = end + 1;
                }
                return {};
            }

            // Tail of the captured output included in errorOut.
            constexpr size_t kErrorTailLength = 512;

            // build-lod derives its output name from the input: it replaces the input's
            // extension with "-lod" and appends ".rad" (see rust/build-lod/src/main.rs).
            // For "<dir>/model.spz" the produced file is "<dir>/model-lod.rad".
            fs::path expectedLodOutput(const fs::path &input)
            {
                fs::path stem = input.stem(); // "model.spz" -> "model"
                return input.parent_path() / io::concat(stem, "-lod.rad");
            }

        } // namespace

        fs::path findBuildLodBinary(bool forceRefresh)
        {
            std::lock_guard<std::mutex> lock(g_cacheMutex);
            if (!forceRefresh && g_cacheValid) return g_cachedBinary;

            fs::path found;

            // 1. Explicit override via env var (authoritative when non-empty).
            if (const auto override = io::getEnvUtf8("DDB_BUILDLOD_PATH"))
            {
                if (!override->empty())
                {
                    const fs::path p = io::fromUtf8(*override);
                    if (isExecutableFile(p)) found = p;
                    g_cachedBinary = found;
                    g_cacheValid = true;
                    if (!found.empty())
                        LOGD << "build-lod binary discovered via DDB_BUILDLOD_PATH at " << io::toUtf8(found);
                    else
                        LOGD << "DDB_BUILDLOD_PATH=" << *override << " is not an executable file; treating build-lod as unavailable";
                    return found;
                }
            }

            // 2. Folder of the current executable (where packaging bundles build-lod).
            {
                fs::path exeDir = io::getExeFolderPath();
                if (!exeDir.empty())
                {
                    fs::path candidate = exeDir / kBinaryName;
                    if (isExecutableFile(candidate)) found = candidate;
                }
            }

            // 3. <install_prefix>/bin (parent of exe folder + /bin).
            if (found.empty())
            {
                fs::path exeDir = io::getExeFolderPath();
                if (!exeDir.empty() && exeDir.has_parent_path())
                {
                    fs::path candidate = exeDir.parent_path() / "bin" / kBinaryName;
                    if (isExecutableFile(candidate)) found = candidate;
                }
            }

            // 4. System PATH.
            if (found.empty()) found = searchOnPath();

            g_cachedBinary = found;
            g_cacheValid = true;

            if (!found.empty())
                LOGD << "build-lod binary discovered at " << io::toUtf8(found);
            else
                LOGD << "build-lod binary not found in DDB_BUILDLOD_PATH, exe folder, install prefix or PATH";

            return found;
        }

        bool isBuildLodAvailable()
        {
            return !findBuildLodBinary().empty();
        }

        bool runBuildLod(const fs::path &input,
                         const fs::path &outputRad,
                         std::string &errorOut,
                         bool quality,
                         int maxSh,
                         const fs::path &binary)
        {
            const auto result = runBuildLodResult(input, outputRad, quality, maxSh, binary);
            errorOut = result.error;
            return result.succeeded;
        }

        ToolRunResult runBuildLodResult(const fs::path &input,
                                       const fs::path &outputRad,
                                       bool quality,
                                       int maxSh,
                                       const fs::path &binary,
                                       const process::Options &options)
        {
            ToolRunResult result;
            process::Options popts = options;
            if (const char *raw = std::getenv("DDB_BUILDLOD_IDLE_TIMEOUT_SECONDS"))
            {
                if (*raw)
                {
                    using Milliseconds = std::chrono::milliseconds;
                    constexpr uint64_t limit = 86400; // 24 hours; keeps steady-clock deadlines representable.
                    uint64_t seconds = 0;
                    bool valid = true;
                    for (const char *p = raw; *p; ++p)
                    {
                        if (*p < '0' || *p > '9' || seconds > (limit - static_cast<uint64_t>(*p - '0')) / 10)
                        {
                            valid = false;
                            break;
                        }
                        seconds = seconds * 10 + static_cast<uint64_t>(*p - '0');
                    }
                    if (!valid || seconds == 0)
                    {
                        result.error = "Invalid DDB_BUILDLOD_IDLE_TIMEOUT_SECONDS; expected an integer from 1 to 86400 seconds (24 hours)";
                        return result;
                    }
                    popts.idleTimeout = Milliseconds(static_cast<Milliseconds::rep>(seconds * 1000));
                }
            }
            fs::path bin = binary.empty() ? findBuildLodBinary() : binary;
            if (bin.empty() || !isExecutableFile(bin))
            {
                result.error = "build-lod binary not found";
                return result;
            }

            std::error_code ec;
            if (!fs::exists(input, ec) || ec)
            {
                result.errorCode = ec;
                result.error = "build-lod input does not exist: " + io::toUtf8(input);
                return result;
            }

            const fs::path producedRad = expectedLodOutput(input);
            // Preserve the actual error before cleanup can overwrite it.
            auto failIo = [&result](const std::string &operation, const std::error_code &code) {
                result.errorCode = code;
                result.error = operation + ": " + code.message();
                return result;
            };
            fs::remove(producedRad, ec);
            if (ec) return failIo("Cannot remove stale build-lod output", ec);
            fs::remove(outputRad, ec);
            if (ec) return failIo("Cannot remove stale RAD output", ec);
            if (!outputRad.parent_path().empty()) {
                fs::create_directories(outputRad.parent_path(), ec);
                if (ec) return failIo("Cannot create RAD output directory", ec);
            }

            int clampedSh = maxSh < 0 ? 0 : (maxSh > 3 ? 3 : maxSh);
            std::vector<std::string> args = {io::toUtf8(input), quality ? "--quality" : "--quick",
                                            "--max-sh=" + std::to_string(clampedSh), "--rad"};
            LOGD << "Spawning build-lod: " << process::formatCommandLine(bin, args);

            popts.onOutputLine = [](const std::string &line) { LOGD << "[build-lod] " << line; };
            const process::Result res = process::run(bin, args, popts);
            if (res.descendantsTerminated)
                LOGW << "build-lod " << res.describe();
            if (!res.success())
            {
                result.timedOut = res.timedOut;
                std::ostringstream oss;
                oss << "build-lod " << res.describe();
                if (!res.outputTail.empty()) oss << ": " << res.tail(kErrorTailLength);
                result.error = oss.str();
                fs::remove(producedRad, ec);
                return result;
            }

            if (!fs::exists(producedRad, ec) || ec)
            {
                result.errorCode = ec;
                result.error = "build-lod reported success but expected output is missing: " + io::toUtf8(producedRad);
                return result;
            }

            fs::rename(producedRad, outputRad, ec);
            if (ec)
            {
                // Cross-device moves may succeed via copy. An identifiable full disk
                // is terminal for this attempt rather than an unexplained tool failure.
                if (ec == std::errc::no_space_on_device)
                    return failIo("Cannot move build-lod output", ec);
                std::error_code copyEc;
                fs::copy_file(producedRad, outputRad, fs::copy_options::overwrite_existing, copyEc);
                if (copyEc)
                    return failIo("failed to move build-lod output to " + io::toUtf8(outputRad), copyEc);
                fs::remove(producedRad, ec);
            }
            result.succeeded = true;
            return result;
        }

    } // namespace buildlod
} // namespace ddb
