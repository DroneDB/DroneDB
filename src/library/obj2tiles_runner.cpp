/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "obj2tiles_runner.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "build_layout.h"
#include "env.h"
#include "logger.h"
#include "mio.h"
#include "parallel.h"
#include "process_runner.h"
#include "tool_locator.h"
#include "tool_runner_result.h"

namespace ddb
{
    namespace obj2tiles
    {

        namespace
        {

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
                        // --ktx2-threads since v1.8.0 (OpenDroneMap/Obj2Tiles#102);
                        // Obj2Tiles rejects it without --texture-format Ktx2.
                        if (opts.ktx2Threads > 0)
                        {
                            args.push_back("--ktx2-threads");
                            args.push_back(std::to_string(opts.ktx2Threads));
                        }
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

            // A core count: decimal digits with a value from 1 to INT32_MAX; anything else (0,
            // negative, hex, non-numeric, overflow) is nullopt.
            std::optional<unsigned int> parseCoreCount(const std::string &value)
            {
                const auto n = env::parseDecimal(value, 1, INT32_MAX);
                if (!n) return std::nullopt;
                return static_cast<unsigned int>(*n);
            }

            // Reads DDB_OBJ2TILES_MAX_CORES (spec C1): a core count (parseCoreCount); anything
            // else is logged and treated as unset. The value is clamped to the hardware
            // concurrency. Returns the effective cap, or nullopt for "no cap".
            std::optional<unsigned int> readMaxCores()
            {
                const auto n = env::readIntegerOrWarn("DDB_OBJ2TILES_MAX_CORES", 1, INT32_MAX);
                if (!n) return std::nullopt;

                auto cap = static_cast<unsigned int>(*n);
                const auto hw = static_cast<unsigned int>(parallel::hardwareThreads());
                if (cap > hw)
                {
                    LOGD << "DDB_OBJ2TILES_MAX_CORES=" << cap << " exceeds the " << hw
                         << " available cores; clamping";
                    cap = hw;
                }
                return cap;
            }


            // Public calls require a fresh destination. Otherwise a successful no-output tool could
            // make an old tileset look like new output. "" when outDir is absent or an empty
            // directory, else why not.
            std::string checkFreshOutput(const fs::path &outDir)
            {
                std::error_code ec;
                const bool outputExists = fs::exists(outDir, ec);
                if (ec)
                    return "could not inspect output directory '" + io::toUtf8(outDir) + "': " + ec.message();
                if (outputExists)
                {
                    const bool directory = fs::is_directory(outDir, ec);
                    const bool empty = directory && !ec && fs::is_empty(outDir, ec);
                    if (ec || !empty)
                        return "Obj2Tiles output directory must be absent or empty: '" + io::toUtf8(outDir) +
                               "'. Use a fresh staging directory" + (ec ? ": " + ec.message() : ".");
                }
                return {};
            }

            // The cores the child may use
            struct CoreCap
            {
                std::optional<unsigned int> maxCores; // DOTNET_PROCESSOR_COUNT to set
                bool capped = false;                  // the child runs under a core count (set or inherited)
            };

            // The cap of the child: DDB_OBJ2TILES_MAX_CORES (else every core) bounded by what the
            // pool has free (`cores`). A cap is set when the variable is or when the pool gives
            // fewer cores than the host has (`hw`); without the variable, an inherited
            // DOTNET_PROCESSOR_COUNT at or below the pool's keeps its value.
            CoreCap decideCoreCap(const std::optional<unsigned int> &requestedCap, unsigned int hw, unsigned int cores)
            {
                const char *inheritedCount = std::getenv("DOTNET_PROCESSOR_COUNT");
                CoreCap cap;
                if (requestedCap || cores < hw)
                    cap.maxCores = cores;
                if (cap.maxCores && !requestedCap && inheritedCount)
                {
                    const auto inherited = parseCoreCount(inheritedCount);
                    if (inherited && *inherited <= cores)
                        cap.maxCores.reset();
                }
                cap.capped = cap.maxCores || (inheritedCount && inheritedCount[0] != '\0');
                return cap;
            }

            // After a run that exited 0: logs the finish and checks that a tileset.json was written, so
            // a tool that succeeded without output is not taken for one that produced a tileset
            bool checkTileset(const fs::path &outDir, const process::Result &res, std::string &errorOut)
            {
                {
                    std::ostringstream oss;
                    oss.imbue(std::locale::classic());
                    oss << "Obj2Tiles finished: exit code 0 in " << std::fixed << std::setprecision(1)
                        << (static_cast<double>(res.duration.count()) / 1000.0) << " s, " << res.totalBytes
                        << " bytes of output";
                    LOGI << oss.str();
                }

                std::error_code ec;
                const fs::path tilesetPath = outDir / TilesetFileName;
                if (!fs::is_regular_file(tilesetPath, ec) || ec || fs::file_size(tilesetPath, ec) == 0 || ec)
                {
                    std::ostringstream oss;
                    oss << "Obj2Tiles reported success but tileset.json is missing or empty: " << io::toUtf8(tilesetPath);
                    if (!res.outputTail.empty()) oss << ": " << res.tail(kErrorTailLength);
                    errorOut = oss.str();

                    LOGI << "Obj2Tiles exited with code 0 without writing " << io::toUtf8(tilesetPath)
                         << (res.outputTail.empty() ? std::string(", no output")
                                                    : ", output tail:\n" + res.tail(process::kDefaultOutputCapacity));
                    return false;
                }
                return true;
            }
        } // namespace

        fs::path findObj2TilesBinary(bool forceRefresh)
        {
            return tools::findTool("Obj2Tiles", "DDB_OBJ2TILES_PATH", forceRefresh);
        }

        bool runObj2Tiles(const fs::path &inputObj,
                           const fs::path &outDir,
                           const Obj2TilesOptions &opts,
                           std::string &errorOut,
                           const fs::path &binary,
                           const process::IProcessRunner &runner)
        {
            errorOut.clear();

            std::error_code ec;
            errorOut = checkFreshOutput(outDir);
            if (!errorOut.empty())
                return false;

            const fs::path bin = binary.empty() ? findObj2TilesBinary() : binary;
            if (!tools::isExecutableFile(bin))
            {
                errorOut = "Obj2Tiles binary not found";
                return false;
            }

            if (!fs::exists(inputObj, ec))
            {
                errorOut = "input OBJ does not exist: " + io::toUtf8(inputObj);
                return false;
            }

            // Make sure the output directory exists (Obj2Tiles also creates it, but we
            // want a clean, existing target before launching).
            fs::create_directories(outDir, ec);
            if (ec)
            {
                errorOut = "could not create output directory '" + io::toUtf8(outDir) +
                           "': " + ec.message();
                return false;
            }

            // Per-child core cap (C1): only the child's environment is changed. The child's
            // cores are CPU tokens (DQ-15), held while it runs.
            const auto requestedCap = readMaxCores();
            const auto hw = static_cast<unsigned int>(parallel::hardwareThreads());
            const auto cpu = parallel::tryAcquireThreads(requestedCap ? *requestedCap : hw);
            const CoreCap cap = decideCoreCap(requestedCap, hw, static_cast<unsigned int>(cpu.threads()));
            const auto &maxCores = cap.maxCores;
            const bool coreCapped = cap.capped;

            // libktx encoder threads are native: DOTNET_PROCESSOR_COUNT does not
            // bound them, so they would multiply past any core cap.
            Obj2TilesOptions effective = opts;
            if (coreCapped && effective.ktx2Threads > 0)
            {
                LOGD << "Obj2Tiles core cap set: dropping --ktx2-threads " << effective.ktx2Threads;
                effective.ktx2Threads = 0;
            }

            std::vector<std::string> args;
            args.push_back(io::toUtf8(inputObj)); // positional: Input
            args.push_back(io::toUtf8(outDir));   // positional: Output
            appendOptionArgs(args, effective);

            LOGD << "Spawning Obj2Tiles: " << process::formatCommandLine(bin, args);

            process::Options popts;
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

            const process::Result res = runner.run(bin, args, popts);

            if (const auto failure = ToolRunResult::fromProcess("Obj2Tiles", res, kErrorTailLength); !failure.succeeded)
            {
                errorOut = failure.error;

                LOGI << "Obj2Tiles failed: " << res.describe()
                     << (res.outputTail.empty() ? std::string(", no output")
                                                : ", output tail:\n" + res.tail(process::kDefaultOutputCapacity));
                return false;
            }

            return checkTileset(outDir, res, errorOut);
        }

    } // namespace obj2tiles
} // namespace ddb
