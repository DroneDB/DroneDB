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
#include "tilers_raster.h"
#include "buildlock.h"
#include "database.h"
#include "dbops.h"
#include "entry_handler.h"

#include <functional>
#include <numeric>
#include <array>
#include <atomic>

namespace ddb
{
    namespace raster
    {

    json photoGcps(const PhotoGeoreference &photo, double scale)
    {
        // The corners of the photo, scaled as gdal_translate -outsize scales it
        const int width = static_cast<int>(static_cast<double>(photo.width) * scale);
        const int height = static_cast<int>(static_cast<double>(photo.height) * scale);
        const std::array<std::array<int, 2>, 4> pixels{{{0, 0}, {0, height}, {width, height}, {width, 0}}};
        json gcps = json::array();
        for (size_t corner = 0; corner < pixels.size(); corner++)
            gcps.push_back(json::array({pixels[corner][0], pixels[corner][1], photo.footprint[corner][0],
                                        photo.footprint[corner][1]}));
        return gcps;
    }

    void buildWithTilers(const fs::path &path, const fs::path &outFile, const std::optional<int> &threads,
                         json options, const ProgressCallback &reportProgress,
                         const std::function<void(const std::string &)> &unreadable)
    {
        BuildOptions buildOptions;
        buildOptions.threads = threads;
        const BuildResourceBudget budget = BuildResourceBudget::fromConfig(buildOptions);
        const auto cpu = parallel::tryAcquireThreads(static_cast<size_t>(budget.threads));
        // The pool of ddb-tilers takes its size before its first build (the helper runs one)
        const std::string config = json{{"threads", cpu.threads()}}.dump();
        if (ddbtConfigure(config.c_str()) != DDBT_ERR_NONE)
        {
            LOGD << "ddb-tilers keeps its pool: " << ddbtLastError();
        }
        const int64_t memoryMb = budget.cacheBytes / (1024 * 1024) + budget.warpMemoryMb;
        options["memoryMb"] = memoryMb;

        LOGD << "Building COG from " << io::toUtf8(path) << " with ddb-tilers (" << cpu.threads() << " threads, "
             << memoryMb << " MB)";
        tilers::BuildProgress progress("COG", reportProgress);
        const ddbtErr rc = ddbtBuildCog2(io::toUtf8(path).c_str(), io::toUtf8(outFile).c_str(), options.dump().c_str(),
                                         &tilers::BuildProgress::forward, &progress);
        tilers::checkBuild(rc, "The COG build", unreadable);
    }

    void buildCogFile(const fs::path &path, const fs::path &outFile, const CogParams &params,
                      const ProgressCallback &reportProgress)
    {
        json options = json::object();
        if (params.photo)
        {
            // A photo is warped from its corners on the ground straight to the tiles
            options["gcps"] = photoGcps(*params.photo, 1.0);
            options["gcpSrs"] = "EPSG:4326";
        }
        if (params.sourceStatistics)
            options["sourceStatistics"] = io::toUtf8(sourceStatsSidecarOf(outFile));
        // An input that cannot be built (no coordinate system, no geotransform, a layout the
        // builder does not read) is a raster that cannot be read, as GDAL's build reported it
        buildWithTilers(path, outFile, params.threads, std::move(options), reportProgress,
                        [](const std::string &message) { throw GDALException(message); });
    }

    } // namespace raster

    namespace
    {
        // Moves the statistics file `of(produced)` the helper wrote beside `target` as `of(target)`.
        // Without it the one that is there describes the COG that was replaced: it goes.
        void publishStatsFile(const fs::path &produced, const fs::path &target, decltype(&statsSidecarOf) of)
        {
            const fs::path stats = of(produced);
            if (fs::exists(stats))
                raster::publishArtefact(stats, of(target));
            else
                io::assureIsRemoved(of(target));
        }
    } // namespace

    void buildCog(const std::string &input, const std::string &outputCog, const BuildOptions &options,
                  bool overwrite, bool sourceStatistics)
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
        buildCogInPrivateFolder(input, produced, options, std::nullopt, sourceStatistics);

        // The output may have appeared while the COG was built: it is replaced only when asked.
        // The COG first: the statistics count only while they are not older than it.
        io::assureReplaceable(io::fromUtf8(outputCog), overwrite);
        raster::publishArtefact(produced, target);
        publishStatsFile(produced, target, statsSidecarOf);
        publishStatsFile(produced, target, sourceStatsSidecarOf);
    }

    void buildCogInPrivateFolder(const std::string &input, const fs::path &produced,
                                 const BuildOptions &options, const std::optional<raster::PhotoGeoreference> &photo,
                                 bool sourceStatistics)
    {
        // The threads of the build come from the CPU pool of this process, which bounds all the
        // builds it runs: the helper has a pool of its own and is told how many it may use. The
        // tokens are held until the helper is done.
        const BuildResourceBudget budget = BuildResourceBudget::fromConfig(options);
        const auto cpu = parallel::tryAcquireThreads(static_cast<size_t>(budget.threads));

        raster::CogParams params;
        params.threads = static_cast<int>(cpu.threads());
        params.photo = photo;
        params.sourceStatistics = sourceStatistics;

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
        // percent, and the builder reports a row of blocks at a time, so a small raster goes from 0 straight
        // to its overviews and the phase would never be reported. 0 is the lowest known fraction: the reports stay monotonic.
        if (hasWarp)
            options.reportProgress(0.0, cogPhaseOfFraction(0.0, true));

        raster::ProgressLog log("COG build");
        const raster::ProgressCallback logProgress = [&log, &options, hasWarp](double fraction) {
            options.reportProgress(fraction, cogPhaseOfFraction(fraction, hasWarp));
            log(fraction);
        };
        raster::rasterHelper().buildCog(io::fromUtf8(input), produced, raster::toJson(params),
                                        logProgress);

        // What the helper wrote is published (as the COG of a build, a tile artefact, the output
        // of an alignment) and then trusted by its place: it has to read as a raster first
        if (!raster::writtenRasterOpens(produced))
            throw GDALException(std::string(raster::kGdalUtilName) + " wrote a COG of " + input +
                                " that cannot be opened");
    }

    CogStatisticsStatus buildCogStatistics(Database* db, const std::string& entryPath, size_t maxThreads,
                                           const std::function<void(double)>& progress)
    {
        cancel::throwIfCanceled();
        const auto entry = getEntry(db, entryPath);
        if (!entry)
            throw EntryNotFoundException("No entry " + entryPath);
        if (entry->type != EntryType::GeoRaster)
            throw InvalidArgsException(entryPath + " is not a GeoRaster");

        const EntryHandler& handler = entryHandler(entry->type);
        const fs::path baseOutputPath = fs::absolute(db->buildDirectory() / io::fromUtf8(handler.buildKey(*entry)));
        std::error_code ec;
        if (!fs::is_directory(baseOutputPath, ec) || ec)
            return CogStatisticsStatus::NotBuilt;

        // The lock of the whole build of a GeoRaster is the one of its COG folder: a build that runs holds it until it
        // has published the COG and its statistics, and none starts while the statistics are written
        const fs::path folder = baseOutputPath / CogBuildSubfolder;
        BuildLock lock(io::toUtf8(folder));
        if (!handler.isComplete(*entry, baseOutputPath))
            return CogStatisticsStatus::NotBuilt;
        const fs::path cog = folder / CogFileName;
        const bool cogMissing = !hasFreshStatsSidecar(cog);
        // The statistics of the uploaded file are those of the file the entry was indexed from: a file
        // replaced since then has a build of its own to come
        const fs::path input = db->rootDirectory() / io::fromUtf8(entry->path);
        const bool sourceMissing = !readSourceStatistics(cog) && isIndexedFile(input, *entry);
        if (!cogMissing && !sourceMissing)
            return CogStatisticsStatus::Present;

        // Each pass reports its half of the work when there are two
        const double share = cogMissing && sourceMissing ? 0.5 : 1.0;
        const auto partOf = [&progress, share](double start) -> std::function<void(double)> {
            if (!progress)
                return {};
            return [&progress, share, start](double fraction) { progress(start + share * fraction); };
        };

        if (cogMissing)
        {
            cancel::throwIfCanceled();
            const auto raster = tilers::Raster::open(cog);
            const auto source = rasterSourceOf(*raster);
            std::vector<int> bands(static_cast<size_t>(source->bandCount()));
            std::iota(bands.begin(), bands.end(), 1);
            const std::string cogPath = io::toUtf8(cog);
            const auto stats = RasterStatsProvider::instance().getAll(cogPath, *source, bands, std::nullopt, true,
                                                                      partOf(0.0), maxThreads);
            // Statistics this process knew already were not written again: the sidecar gets them now
            if (!hasFreshStatsSidecar(cog))
                for (size_t i = 0; i < bands.size(); i++)
                    if (stats[i])
                        writeStatsSidecar(cogPath, bands[i], *stats[i]);
            writeEmptyStatsSidecar(cogPath);
        }

        if (sourceMissing)
        {
            cancel::throwIfCanceled();
            // The uploaded file is read where the files of users are read: by the raster helper, on the
            // CPU tokens of this process
            const size_t wanted =
                maxThreads > 0 ? maxThreads : static_cast<size_t>(parallel::threadsOption("DDB_STATS_THREADS", 8));
            const auto cpu = parallel::tryAcquireThreads(wanted);
            raster::StatisticsParams params;
            params.threads = static_cast<int>(cpu.threads());
            const json result = raster::rasterHelper().operate(raster::op::kStatistics, fs::absolute(input), fs::path(),
                                                               raster::toJson(params),
                                                               partOf(cogMissing ? 0.5 : 0.0));
            StatisticsByBand bands;
            try
            {
                bands = statisticsByBandFromJson(result.at("bands"));
            }
            catch (const std::exception &e)
            {
                throw GDALException(std::string(raster::kGdalUtilName) + " gave statistics of " + entry->path +
                                    " that cannot be used: " + e.what());
            }
            if (!writeSourceStatistics(cog, bands))
                throw FSException("Cannot write the statistics of " + entry->path + " next to its COG");
        }
        if (progress)
            progress(1.0);
        return CogStatisticsStatus::Computed;
    }

    bool isIndexedFile(const fs::path &file, const Entry &e)
    {
        std::error_code ec;
        const auto size = fs::file_size(file, ec);
        if (ec || size != e.size)
            return false;
        try
        {
            return io::Path(file).getModifiedTime() == e.mtime;
        }
        catch (const FSException &)
        {
            return false;
        }
    }

    std::optional<StatisticsByBand> entrySourceStatistics(Database *db, const Entry &e)
    {
        if (e.type != EntryType::GeoRaster)
            return std::nullopt;
        if (!isIndexedFile(db->rootDirectory() / io::fromUtf8(e.path), e))
            return std::nullopt;
        const EntryHandler &handler = entryHandler(e.type);
        const fs::path cog = db->buildDirectory() / io::fromUtf8(handler.buildKey(e)) / CogBuildSubfolder / CogFileName;
        return readSourceStatistics(cog);
    }

}
