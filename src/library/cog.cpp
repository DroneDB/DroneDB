/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "cog.h"
#include "cancellation.h"
#include "build_budget.h"
#include "parallel.h"
#include "cog_utils.h"
#include "raster_helper.h"
#include "logger.h"
#include "build_layout.h"
#include "mio.h"
#include "raster_artefacts.h"
#include "raster_stats.h"
#include "exceptions.h"
#include "json.h"
#include "tilers_call.h"

#include <functional>
#include <atomic>

namespace ddb
{
    namespace
    {
        // The COG of a GeoRaster (GeoTIFF or JPEG 2000), built by ddb-tilers in one streamed pass:
        // Web Mercator tiles, or the source's own grid for an optimized COG; JPEG for 8-bit RGB /
        // RGBA without nodata (DDB_COG_JPEG_QUALITY), LZW with a predictor otherwise; nodata,
        // alpha and mask kept; the statistics sidecar; with the threads of the build and its
        // memory budget (DDB_COG_CACHE_MB + DDB_WARP_MEMORY_MB)
        void buildCogWithTilers(const fs::path &path, const fs::path &outFile, const raster::CogParams &params,
                                    const raster::ProgressCallback &reportProgress)
        {
            BuildOptions options;
            options.threads = params.threads;
            const BuildResourceBudget budget = BuildResourceBudget::fromConfig(options);
            const auto cpu = parallel::tryAcquireThreads(static_cast<size_t>(budget.threads));
            // The pool of ddb-tilers takes its size before its first build (the helper runs one)
            const std::string config = json{{"threads", cpu.threads()}}.dump();
            if (ddbtConfigure(config.c_str()) != DDBT_ERR_NONE)
            {
                LOGD << "ddb-tilers keeps its pool: " << ddbtLastError();
            }
            const int64_t memoryMb = budget.cacheBytes / (1024 * 1024) + budget.warpMemoryMb;
            const std::string buildOptions = json{{"memoryMb", memoryMb}}.dump();

            LOGD << "Building COG from " << io::toUtf8(path) << " with ddb-tilers (" << cpu.threads()
                 << " threads, " << memoryMb << " MB)";
            tilers::BuildProgress progress("COG", reportProgress);
            const ddbtErr rc = ddbtBuildCog2(io::toUtf8(path).c_str(), io::toUtf8(outFile).c_str(),
                                             buildOptions.c_str(), &tilers::BuildProgress::forward, &progress);
            // An input that cannot be built (no coordinate system, no geotransform, a layout the
            // builder does not read) is a raster that cannot be read, as GDAL's build reported it
            tilers::checkBuild(rc, "The COG build", [](const std::string &message) { throw GDALException(message); });
        }
    } // namespace

    namespace raster
    {

    void buildCogFile(const fs::path &path, const fs::path &outFile, const CogParams &params,
                      const ProgressCallback &reportProgress)
    {
        buildCogWithTilers(path, outFile, params, reportProgress);
    }

    } // namespace raster

    namespace
    {
        // Moves the statistics the helper wrote next to `produced` beside `target`. Without them
        // the sidecar that is there describes the COG that was replaced: it goes.
        void publishStatsSidecar(const fs::path &produced, const fs::path &target)
        {
            const fs::path stats = statsSidecarOf(produced);
            if (fs::exists(stats))
                raster::publishArtefact(stats, statsSidecarOf(target));
            else
                io::assureIsRemoved(statsSidecarOf(target));
        }
    } // namespace

    void buildCog(const std::string &input, const std::string &outputCog, const BuildOptions &options,
                  bool overwrite)
    {
        const fs::path target = fs::absolute(io::fromUtf8(outputCog));
        if (!target.has_filename())
            throw InvalidArgsException("Not the path of a file for a COG: " + outputCog);
        io::assureReplaceable(io::fromUtf8(outputCog), overwrite, {io::fromUtf8(input)});

        // The folder of the output holds other files (programs, the files of a user): the helper
        // writes into a folder of its own in it, which is removed with whatever is left in it
        const io::ScopedTempDir staging =
            raster::makePrivateFolder(target.parent_path(), CogStagingPrefix);
        const fs::path produced = staging.path() / target.filename();
        buildCogInPrivateFolder(input, produced, options);

        // The output may have appeared while the COG was built: it is replaced only when asked.
        // The COG first: the statistics count only while they are not older than it.
        io::assureReplaceable(io::fromUtf8(outputCog), overwrite);
        raster::publishArtefact(produced, target);
        publishStatsSidecar(produced, target);
    }

    void buildCogInPrivateFolder(const std::string &input, const fs::path &produced,
                                 const BuildOptions &options)
    {
        // The threads of the build come from the CPU pool of this process, which bounds all the
        // builds it runs: the helper has a pool of its own and is told how many it may use. The
        // tokens are held until the helper is done.
        const BuildResourceBudget budget = BuildResourceBudget::fromConfig(options);
        const auto cpu = parallel::tryAcquireThreads(static_cast<size_t>(budget.threads));

        raster::CogParams params;
        params.threads = static_cast<int>(cpu.threads());

        // The phases of the work depend on whether the helper warps the raster: an optimized COG is
        // encoded again at its own grid and has none. The helper says which in its
        // probe (isCog is the check it makes itself), and only a caller that listens needs it.
        bool hasWarp = true;
        if (options.progress) {
            try {
                hasWarp = !raster::rasterHelper().probe(io::fromUtf8(input), raster::ProbeParams()).isCog;
            } catch (const CanceledException&) {
                throw;
            } catch (const std::exception& e) {
                // The build that follows reports what is wrong with the file
                LOGD << "Cannot probe " << input << " for the phases of its COG build: " << e.what();
            }
        }

        // The warp is announced when it starts: the helper forwards a fraction only once it passes a whole
        // percent, and a small raster warped in one GDAL chunk goes from 0 straight to its overviews, so the
        // phase would never be reported. 0 is the lowest known fraction: the reports stay monotonic.
        if (hasWarp)
            options.reportProgress(0.0, cogPhaseOfFraction(0.0, true));

        std::atomic<int> lastPct{0};
        const raster::ProgressCallback logProgress = [&lastPct, &options, hasWarp](double fraction) {
            options.reportProgress(fraction, cogPhaseOfFraction(fraction, hasWarp));
            const int pct = static_cast<int>(fraction * 100.0);
            int prev = lastPct.load();
            if (pct / 5 != prev / 5 && lastPct.compare_exchange_strong(prev, pct)) {
                LOGD << "COG build progress: " << pct << "%";
            }
        };
        raster::rasterHelper().buildCog(io::fromUtf8(input), produced, raster::toJson(params),
                                        logProgress);

        // The helper read a file of a user, and what it wrote is published (as the COG of a
        // build, a tile artefact, the output of an alignment) and then trusted by its place: it
        // has to open here as a raster first, with the drivers of the artefacts alone. The check
        // closes it before the caller renames it: Windows renames no file a process holds open.
        if (!raster::reopenWrittenRaster(produced))
            throw GDALException(std::string(raster::kGdalUtilName) + " wrote a COG of " + input +
                                " that cannot be opened");
    }

}
