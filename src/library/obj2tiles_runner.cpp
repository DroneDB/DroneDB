/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "obj2tiles_runner.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <locale>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "logger.h"
#include "mio.h"
#include "process_runner.h"

namespace ddb
{
    namespace obj2tiles
    {

        namespace
        {

#ifdef _WIN32
            constexpr const char *kBinaryName = "Obj2Tiles.exe";
            constexpr char kPathSep = ';';
#else
            constexpr const char *kBinaryName = "Obj2Tiles";
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

            // Format a double with the classic (period) locale and enough precision
            // for geographic coordinates. Obj2Tiles is built with InvariantGlobalization,
            // so it parses CLI doubles with a period decimal separator regardless of the
            // host locale; we must therefore never emit a locale-specific separator.
            std::string fmtDouble(double v)
            {
                std::ostringstream oss;
                oss.imbue(std::locale::classic());
                oss << std::setprecision(15) << v;
                return oss.str();
            }

            // Append the CLI arguments derived from opts (everything after the input
            // and output positional arguments).
            void appendOptionArgs(std::vector<std::string> &args, const Obj2TilesOptions &opts)
            {
                args.push_back("--divisions");
                args.push_back(std::to_string(opts.divisions));
                args.push_back("--lods");
                args.push_back(std::to_string(opts.lods));

                if (opts.localMode)
                {
                    args.push_back("--local");
                }
                else
                {
                    if (opts.lat.has_value())
                    {
                        args.push_back("--lat");
                        args.push_back(fmtDouble(*opts.lat));
                    }
                    if (opts.lon.has_value())
                    {
                        args.push_back("--lon");
                        args.push_back(fmtDouble(*opts.lon));
                    }
                    args.push_back("--alt");
                    args.push_back(fmtDouble(opts.alt));
                }

                // Flags --octree/--lod-texture-scale are supported since v1.5.0
                // (OpenDroneMap/Obj2Tiles#95); --texture-format/--ktx2-quality since
                // v1.6.0 (OpenDroneMap/Obj2Tiles#97); --split-strategy since v1.4.0
                // (OpenDroneMap/Obj2Tiles#92). Emit only when set to a non-default (or
                // non-empty) value so the same code path keeps working with older
                // Obj2Tiles binaries.
                if (opts.octree)
                    args.push_back("--octree");
                if (opts.lodTextureScale != 0.5)
                {
                    args.push_back("--lod-texture-scale");
                    args.push_back(fmtDouble(opts.lodTextureScale));
                }
                if (!opts.textureFormat.empty())
                {
                    args.push_back("--texture-format");
                    args.push_back(opts.textureFormat);
                    if (opts.textureFormat == "Ktx2")
                    {
                        args.push_back("--ktx2-quality");
                        args.push_back(std::to_string(opts.ktx2Quality));
                    }
                }
                if (!opts.splitStrategy.empty())
                {
                    args.push_back("--split-strategy");
                    args.push_back(opts.splitStrategy);
                }
            }

            // Tail of the captured output included in errorOut.
            constexpr size_t kErrorTailLength = 600;

            // Reads DDB_OBJ2TILES_MAX_CORES (spec C1). Valid values are non-empty
            // strings of decimal digits with a value >= 1; anything else (0,
            // negative, hex, non-numeric, overflow) is logged and treated as unset.
            // The value is clamped to the hardware concurrency when known.
            // Returns the effective cap, or nullopt for "no cap".
            std::optional<unsigned int> readMaxCores()
            {
                const char *raw = std::getenv("DDB_OBJ2TILES_MAX_CORES");
                if (!raw || raw[0] == '\0') return std::nullopt;

                const std::string value(raw);
                uint64_t n = 0;
                bool valid = true;
                for (char c : value)
                {
                    if (c < '0' || c > '9')
                    {
                        valid = false;
                        break;
                    }
                    n = n * 10 + static_cast<uint64_t>(c - '0');
                    if (n > static_cast<uint64_t>(INT32_MAX))
                    {
                        valid = false; // overflow
                        break;
                    }
                }
                if (!valid || n == 0)
                {
                    LOGW << "Ignoring invalid DDB_OBJ2TILES_MAX_CORES=\"" << value
                         << "\" (expected a positive decimal integer)";
                    return std::nullopt;
                }

                unsigned int cap = static_cast<unsigned int>(n);
                const unsigned int hw = std::thread::hardware_concurrency();
                if (hw > 0 && cap > hw)
                {
                    LOGD << "DDB_OBJ2TILES_MAX_CORES=" << value << " exceeds the " << hw
                         << " available cores; clamping";
                    cap = hw;
                }
                return cap;
            }

        } // namespace

        fs::path findObj2TilesBinary(bool forceRefresh)
        {
            std::lock_guard<std::mutex> lock(g_cacheMutex);
            if (!forceRefresh && g_cacheValid) return g_cachedBinary;

            fs::path found;

            // 1. Explicit override via env var. When DDB_OBJ2TILES_PATH is set
            // (non-empty), it is authoritative: either it points to a usable binary
            // or no binary is considered available (so callers can pin the tool
            // deterministically and tests can simulate "missing").
            if (const char *override = std::getenv("DDB_OBJ2TILES_PATH"))
            {
                if (override[0] != '\0')
                {
                    fs::path p(override);
                    if (isExecutableFile(p)) found = p;
                    g_cachedBinary = found;
                    g_cacheValid = true;
                    if (!found.empty())
                        LOGD << "Obj2Tiles binary discovered via DDB_OBJ2TILES_PATH at " << found.string();
                    else
                        LOGD << "DDB_OBJ2TILES_PATH=" << override << " is not an executable file; treating Obj2Tiles as unavailable";
                    return found;
                }
            }

            // 2. Folder of the current executable (where Obj2Tiles is bundled by build/packaging)
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
                LOGD << "Obj2Tiles binary discovered at " << found.string();
            else
                LOGD << "Obj2Tiles binary not found in DDB_OBJ2TILES_PATH, exe folder, install prefix or PATH";

            return found;
        }

        bool runObj2Tiles(const fs::path &inputObj,
                           const fs::path &outDir,
                           const Obj2TilesOptions &opts,
                           std::string &errorOut,
                           const fs::path &binary)
        {
            errorOut.clear();

            fs::path bin = binary.empty() ? findObj2TilesBinary() : binary;
            if (bin.empty() || !isExecutableFile(bin))
            {
                errorOut = "Obj2Tiles binary not found";
                return false;
            }

            std::error_code ec;
            if (!fs::exists(inputObj, ec))
            {
                errorOut = "input OBJ does not exist: " + inputObj.string();
                return false;
            }

            // Make sure the output directory exists (Obj2Tiles also creates it, but we
            // want a clean, existing target before launching).
            fs::create_directories(outDir, ec);
            if (ec)
            {
                errorOut = "could not create output directory '" + outDir.string() +
                           "': " + ec.message();
                return false;
            }

            std::vector<std::string> args;
            args.push_back(inputObj.string()); // positional: Input
            args.push_back(outDir.string());   // positional: Output
            appendOptionArgs(args, opts);

            LOGD << "Spawning Obj2Tiles: " << process::formatCommandLine(bin, args);

            // Per-child core cap (C1): only the child's environment is changed.
            process::Options popts;
            const auto maxCores = readMaxCores();
            if (maxCores)
            {
                popts.envOverrides.emplace_back("DOTNET_PROCESSOR_COUNT", std::to_string(*maxCores));
                LOGI << "Obj2Tiles core cap: " << *maxCores;
            }
            else
            {
                LOGI << "Obj2Tiles core cap: none";
            }
            // Full output at debug level, line by line as it arrives.
            popts.onOutputLine = [](const std::string &line) { LOGD << "[Obj2Tiles] " << line; };

            const process::Result res = process::run(bin, args, popts);

            if (!res.success())
            {
                std::ostringstream oss;
                oss << "Obj2Tiles " << res.describe();
                if (!res.outputTail.empty()) oss << ": " << res.tail(kErrorTailLength);
                errorOut = oss.str();

                LOGI << "Obj2Tiles failed: " << res.describe()
                     << (res.outputTail.empty() ? std::string(", no output")
                                                : ", output tail:\n" + res.tail(process::kDefaultOutputCapacity));
                return false;
            }

            {
                std::ostringstream oss;
                oss.imbue(std::locale::classic());
                oss << "Obj2Tiles finished: exit code 0 in " << std::fixed << std::setprecision(1)
                    << (static_cast<double>(res.duration.count()) / 1000.0) << " s, " << res.totalBytes
                    << " bytes of output";
                LOGI << oss.str();
            }

            const fs::path tilesetPath = outDir / "tileset.json";
            if (!fs::exists(tilesetPath, ec))
            {
                std::ostringstream oss;
                oss << "Obj2Tiles reported success but tileset.json is missing: " << tilesetPath.string();
                if (!res.outputTail.empty()) oss << ": " << res.tail(kErrorTailLength);
                errorOut = oss.str();

                LOGI << "Obj2Tiles exited with code 0 without writing " << tilesetPath.string()
                     << (res.outputTail.empty() ? std::string(", no output")
                                                : ", output tail:\n" + res.tail(process::kDefaultOutputCapacity));
                return false;
            }

            return true;
        }

    } // namespace obj2tiles
} // namespace ddb
