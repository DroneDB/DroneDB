/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "buildlod_runner.h"

#include <cstdlib>
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
            constexpr size_t kErrorTailLength = 512;

            // build-lod derives its output name from the input: it replaces the input's
            // extension with "-lod" and appends ".rad" (see rust/build-lod/src/main.rs).
            // For "<dir>/model.spz" the produced file is "<dir>/model-lod.rad".
            fs::path expectedLodOutput(const fs::path &input)
            {
                fs::path stem = input.stem(); // "model.spz" -> "model"
                return input.parent_path() / (stem.string() + "-lod.rad");
            }

        } // namespace

        fs::path findBuildLodBinary(bool forceRefresh)
        {
            std::lock_guard<std::mutex> lock(g_cacheMutex);
            if (!forceRefresh && g_cacheValid) return g_cachedBinary;

            fs::path found;

            // 1. Explicit override via env var (authoritative when non-empty).
            if (const char *override = std::getenv("DDB_BUILDLOD_PATH"))
            {
                if (override[0] != '\0')
                {
                    fs::path p(override);
                    if (isExecutableFile(p)) found = p;
                    g_cachedBinary = found;
                    g_cacheValid = true;
                    if (!found.empty())
                        LOGD << "build-lod binary discovered via DDB_BUILDLOD_PATH at " << found.string();
                    else
                        LOGD << "DDB_BUILDLOD_PATH=" << override << " is not an executable file; treating build-lod as unavailable";
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
                LOGD << "build-lod binary discovered at " << found.string();
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
            errorOut.clear();

            fs::path bin = binary.empty() ? findBuildLodBinary() : binary;
            if (bin.empty() || !isExecutableFile(bin))
            {
                errorOut = "build-lod binary not found";
                return false;
            }

            std::error_code ec;
            if (!fs::exists(input, ec) || ec)
            {
                errorOut = "build-lod input does not exist: " + input.string();
                return false;
            }

            const fs::path producedRad = expectedLodOutput(input);

            // Clean any stale outputs so we never mistake a leftover for a fresh build.
            fs::remove(producedRad, ec);
            fs::remove(outputRad, ec);
            fs::create_directories(outputRad.parent_path(), ec);

            // Clamp SH degree to the range build-lod accepts (0..3).
            int clampedSh = maxSh < 0 ? 0 : (maxSh > 3 ? 3 : maxSh);

            std::vector<std::string> args;
            args.push_back(input.string());
            args.push_back(quality ? "--quality" : "--quick");
            args.push_back("--max-sh=" + std::to_string(clampedSh));
            args.push_back("--rad"); // single-file RAD (HTTP range friendly)

            LOGD << "Spawning build-lod: " << process::formatCommandLine(bin, args);

            // stdout+stderr are captured and forwarded to the DDB log line by line.
            process::Options popts;
            popts.onOutputLine = [](const std::string &line) { LOGD << "[build-lod] " << line; };

            const process::Result res = process::run(bin, args, popts);
            if (!res.success())
            {
                std::ostringstream oss;
                oss << "build-lod " << res.describe();
                // Surface the tail of the captured output to aid diagnostics.
                if (!res.outputTail.empty()) oss << ": " << res.tail(kErrorTailLength);
                errorOut = oss.str();
                fs::remove(producedRad, ec);
                return false;
            }

            if (!fs::exists(producedRad, ec) || ec)
            {
                errorOut = "build-lod reported success but expected output is missing: " +
                           producedRad.string();
                return false;
            }

            // Move the "<stem>-lod.rad" produced by build-lod to the canonical model.rad.
            fs::remove(outputRad, ec);
            fs::rename(producedRad, outputRad, ec);
            if (ec)
            {
                // Cross-device or other rename failure: fall back to copy + remove.
                std::error_code copyEc;
                fs::copy_file(producedRad, outputRad, fs::copy_options::overwrite_existing, copyEc);
                fs::remove(producedRad, ec);
                if (copyEc)
                {
                    errorOut = "failed to move build-lod output to " + outputRad.string() +
                               ": " + copyEc.message();
                    return false;
                }
            }

            return true;
        }

    } // namespace buildlod
} // namespace ddb
