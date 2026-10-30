/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "gdal_open_policy.h"
#include "gdal_raii.h"
#include "gtiff_options.h"
#include "raster_utils.h"
#include "cog.h"
#include "cancellation.h"
#include "build_budget.h"
#include "parallel.h"
#include "cog_utils.h"
#include "raster_helper.h"
#include "raster_ops.h"
#include "logger.h"
#include "mio.h"
#include "raster_artefacts.h"
#include "raster_stats.h"
#include "exceptions.h"
#include "json.h"

#include <fstream>
#include <optional>
#include <cerrno>
#include <cstring>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace ddb
{
    // Reports the warp to the caller at every whole percent and logs it every 5%, to aid
    // diagnostics. Uses an atomic counter to throttle (GDAL may call this very frequently, from
    // its own threads too).
    static void reportWarpProgress(std::atomic<int>& lastPct, double dfComplete, const raster::ProgressCallback& progress) {
        const int pct = static_cast<int>(dfComplete * 100.0);
        int prev = lastPct.load();
        if (pct < prev + 1 || !lastPct.compare_exchange_strong(prev, pct))
            return;
        if (progress)
            progress(dfComplete);
        if (pct / 5 != prev / 5) {
            LOGD << "COG warp progress: " << pct << "%";
        }
    }

    // How GDALWarp keeps the transparency of the source: its nodata value, or a destination alpha
    // band when the nodata cannot be written or the source has a stand-alone mask
    static void appendTransparencyArgs(CPLStringList &targs, GDALRasterBandH hSrcBand1,
                                       const BandNodata &srcNoData)
    {
        const bool hasNoData = srcNoData.has;

        // Some GDAL versions / code paths may fail to preserve a standalone
        // PER_DATASET mask when a COG is produced through a warp/reprojection step
        // (observed with GDAL 3.11.x on files produced by nearblack/maskBorders),
        // causing masked borders to become opaque black pixels, especially with
        // JPEG/YCbCr compression.
        // To make COG generation deterministic
        // across GDAL versions, when the source has a stand-alone PER_DATASET
        // mask (not derived from an alpha band nor from a nodata value, which
        // are handled by other code paths), we force GDALWarp to materialize
        // the transparency as a destination alpha band. The COG driver then
        // serializes that alpha as an internal 1-bit mask (MSK IFD) for JPEG
        // output, preserving transparency without re-encoding alpha as JPEG.
        const int srcMaskFlags = GDALGetMaskFlags(hSrcBand1);
        const bool hasPerDatasetMask =
            (srcMaskFlags & GMF_PER_DATASET) != 0 &&
            (srcMaskFlags & GMF_ALPHA) == 0 &&
            (srcMaskFlags & GMF_NODATA) == 0;

        // Preserve nodata values for transparency
        if (hasNoData) {
            // Skip nodata preservation for special float values that cannot
            // be reliably serialized (infinity, NaN, extreme floats)
            if (std::isfinite(srcNoData.value) && std::fabs(srcNoData.value) < 1e+30) {
                targs.AddString("-wo");
                targs.AddString("UNIFIED_SRC_NODATA=YES");
                targs.AddString("-dstnodata");
                std::ostringstream oss;
                oss << std::setprecision(15) << srcNoData.value;
                targs.AddString(oss.str().c_str());
            } else {
                // The nodata value cannot be reliably serialized as a
                // -dstnodata string (NaN/Inf/extreme float). Materialize the
                // transparency as a destination alpha band instead, so masked
                // pixels do not become opaque in the output COG. GDALWarp still
                // reads the source nodata from the band itself to drive the mask.
                LOGW << "Nodata value cannot be serialized (" << srcNoData.value
                     << "); using -dstalpha to preserve transparency";
                targs.AddString("-dstalpha");
            }
        } else if (hasPerDatasetMask) {
            // Materialize the source PER_DATASET mask as a destination alpha
            // band so transparency survives the COG reprojection step. The COG
            // driver converts the alpha back into an internal 1-bit mask for
            // JPEG output (no alpha-in-JPEG cost) and into a 4th band for LZW.
            LOGD << "Source has PER_DATASET mask; forcing -dstalpha to preserve transparency";
            targs.AddString("-dstalpha");
        }
    }

    // The JPEG quality of a COG: DDB_COG_JPEG_QUALITY, 75 (GDAL's default) when it is not set
    static int configuredJpegQuality()
    {
        const char* envQuality = CPLGetConfigOption("DDB_COG_JPEG_QUALITY", nullptr);
        return envQuality ? std::clamp(std::atoi(envQuality), 1, 100) : 75;
    }

    // True when the COG of the source can be JPEG: 8-bit RGB, or 8-bit RGBA whose 4th band is
    // alpha, without nodata
    static bool jpegEligible(GDALDatasetH hSrcDataset, bool hasNoData)
    {
        const int numBands = GDALGetRasterCount(hSrcDataset);
        if ((numBands != 3 && numBands != 4) || hasNoData)
            return false;

        for (int n = 0; n < numBands; n++)
        {
            if (GDALGetRasterDataType(GDALGetRasterBand(hSrcDataset, n + 1)) != GDT_Byte)
                return false;
        }
        // Only treat a 4-band raster as JPEG-eligible RGBA when the 4th band
        // is actually an alpha channel. A 4-band RGBN raster (e.g. with a NIR
        // band) must NOT be JPEG-compressed: the COG/JPEG path treats band 4
        // as alpha and would lossily corrupt the spectral data. Fall back to
        // LZW for such datasets.
        return numBands == 3 ||
               GDALGetRasterColorInterpretation(GDALGetRasterBand(hSrcDataset, 4)) == GCI_AlphaBand;
    }

    // The GTiff compression of the COG: JPEG of `jpegQuality` when it is set, LZW otherwise
    static GtiffOptions cogCompression(std::optional<int> jpegQuality)
    {
        GtiffOptions creation;
        if (jpegQuality)
        {
            // PREDICTOR is not valid for JPEG and is intentionally omitted.
            creation.compress(GtiffOptions::Codec::Jpeg).set("QUALITY", std::to_string(*jpegQuality));
        }
        else
        {
            // LZW by default for data with nodata values, other band counts or non-8bit data
            creation.compress(GtiffOptions::Codec::Lzw).predictorAuto();
        }

        // BigTIFF
        creation.bigTiffIfSafer();
        return creation;
    }

    // The GTiff compression of a COG warped to the tile grid: JPEG for 8-bit RGB and RGBA (no
    // nodata), LZW otherwise
    static GtiffOptions cogCreationOptions(GDALDatasetH hSrcDataset, bool hasNoData)
    {
        return cogCompression(jpegEligible(hSrcDataset, hasNoData) ? std::optional<int>(configuredJpegQuality())
                                                                   : std::nullopt);
    }

    // The JPEG quality of a source compressed with JPEG ("JPEG", "YCbCr JPEG"): the one GDAL
    // estimates from its quantization tables, or the configured one when GDAL reports none. None
    // for a source with any other compression.
    static std::optional<int> sourceJpegQuality(GDALDatasetH hSrcDataset)
    {
        const char* compression = GDALGetMetadataItem(hSrcDataset, "COMPRESSION", "IMAGE_STRUCTURE");
        if (!compression || !strstr(compression, "JPEG"))
            return std::nullopt;
        const char* quality = GDALGetMetadataItem(hSrcDataset, "JPEG_QUALITY", "IMAGE_STRUCTURE");
        const int reported = quality ? std::atoi(quality) : 0;
        return reported > 0 ? std::min(reported, 100) : configuredJpegQuality();
    }

    // The GTiff compression of an optimized COG encoded again at its own grid. The source keeps
    // its class of compression, so a source without loss is pixel-equal after the new encoding
    // (warping it would compress 8-bit RGB with JPEG): JPEG only when the source is JPEG already,
    // at its own quality, which moves a pixel by a fraction of a level on average where the
    // default quality of a warp would move it by several, and only for three bands, as the COG
    // driver would turn a 4th, alpha band of a JPEG into a mask; LZW otherwise.
    static GtiffOptions cogOwnGridCreationOptions(GDALDatasetH hSrcDataset, bool hasNoData)
    {
        const std::optional<int> quality = sourceJpegQuality(hSrcDataset);
        const bool jpeg = quality && GDALGetRasterCount(hSrcDataset) == 3 && jpegEligible(hSrcDataset, hasNoData);
        return cogCompression(jpeg ? quality : std::nullopt);
    }

    // The arguments of the GDALWarp that writes the COG: Web Mercator, GoogleMapsCompatible tiles,
    // the threads and warp memory of the budget, transparency and compression of the source.
    // -multi reads the source on a GDAL worker thread, which does not see a thread-local option:
    // left out when the reads must stay on this thread (multiThreadedIo false).
    static CPLStringList cogWarpArgs(GDALDatasetH hSrcDataset, GDALRasterBandH hSrcBand1,
                                     const BuildResourceBudget &budget, const std::string &numThreads,
                                     bool multiThreadedIo)
    {
        CPLStringList targs;
        targs.AddString("-of");
        targs.AddString("COG");
        targs.AddString("-t_srs");
        targs.AddString("EPSG:3857");
        if (multiThreadedIo)
            targs.AddString("-multi");
        targs.AddString("-wo");
        targs.AddString(numThreads.c_str());
        targs.AddString("-co");
        targs.AddString(numThreads.c_str());
        targs.AddString("-r");
        targs.AddString("bilinear");
        targs.AddString("-co");
        targs.AddString("TILING_SCHEME=GoogleMapsCompatible");
        // UPPER strategy: when the theoretical zoom level is non-integer,
        // select the higher (more detailed) zoom level rather than rounding
        // to the nearest. This preserves detail for high-res drone imagery
        targs.AddString("-co");
        targs.AddString("ZOOM_LEVEL_STRATEGY=UPPER");

        // Warp memory buffer: larger values reduce chunk overhead, especially with NUM_THREADS>1.
        targs.AddString("-wm");
        targs.AddString(std::to_string(budget.warpMemoryMb).c_str());

        const BandNodata srcNoData = readBandNodata(hSrcBand1);
        appendTransparencyArgs(targs, hSrcBand1, srcNoData);
        cogCreationOptions(hSrcDataset, srcNoData.has).appendTo(targs);
        return targs;
    }

    // The arguments of the GDALTranslate that encodes an optimized COG again at its own grid: the
    // COG driver with the size, geotransform, coordinate system, bands and tiles of the source.
    // Every pixel is decoded and encoded again, and the overviews (and those of the mask) are
    // computed by GDAL from them, never copied from the source.
    static CPLStringList cogOwnGridArgs(GDALDatasetH hSrcDataset, GDALRasterBandH hSrcBand1,
                                        const std::string &numThreads)
    {
        int blockXSize = 0, blockYSize = 0;
        GDALGetBlockSize(hSrcBand1, &blockXSize, &blockYSize);

        CPLStringList targs;
        targs.AddString("-of");
        targs.AddString("COG");
        targs.AddString("-co");
        targs.AddString(numThreads.c_str());
        targs.AddString("-co");
        targs.AddString(("BLOCKSIZE=" + std::to_string(blockXSize)).c_str());
        targs.AddString("-co");
        targs.AddString("OVERVIEWS=IGNORE_EXISTING");
        cogOwnGridCreationOptions(hSrcDataset, readBandNodata(hSrcBand1).has).appendTo(targs);
        return targs;
    }

    namespace raster
    {

    void buildCogFile(const fs::path &path, const fs::path &outFile, const CogParams &params,
                      const ProgressCallback &reportProgress)
    {
        const std::string input = io::toUtf8(path);
        const std::string outputCog = io::toUtf8(outFile);
        BuildOptions options;
        options.threads = params.threads;

        // A JPEG 2000 decodes on threads of its own, one per CPU unless GDAL_NUM_THREADS says
        // otherwise, and starts them when it is opened. It is never a COG: no open before the
        // budget below is in place.
        const GDALDriverH sourceDriver = identifyUserRaster(input);
        const bool ownDecodeThreads = sourceDriver && decodesWithOwnThreads(GDALGetDriverShortName(sourceDriver));

        // Threads, warp memory and block cache come from one budget so
        // that several builds running in one process stay within known bounds;
        // the threads beyond this one take CPU tokens, held until the
        // output is closed.
        const BuildResourceBudget budget = BuildResourceBudget::fromConfig(options);
        const auto cpu = parallel::tryAcquireThreads(static_cast<size_t>(budget.threads));
        const std::string numThreads = parallel::gdalNumThreads(cpu);

        // A decoder with threads of its own gets the threads of the build, scoped to this thread
        // before the source is opened, and its reads stay on this thread (no -multi) so they see
        // that option
        std::optional<ScopedThreadConfigOption> decodeThreadsGuard;
        if (ownDecodeThreads)
            decodeThreadsGuard.emplace("GDAL_NUM_THREADS", std::to_string(cpu.threads()).c_str());

        LOGD << "Building COG from " << input;

        // Owned: the option lists and the options of the GDAL utilities below can throw
        auto srcDataset = openUserRaster(input);

        if (!srcDataset)
            throw GDALException("Cannot open " + input + " for reading");
        GDALDatasetH hSrcDataset = datasetHandle(srcDataset);

        // Check that the source has a valid CRS
        const char* srcProjRef = GDALGetProjectionRef(hSrcDataset);
        if (!srcProjRef || strlen(srcProjRef) == 0)
            throw GDALException("Cannot build COG: input file has no coordinate reference system: " + input);

        // Detect and preserve nodata from source
        GDALRasterBandH hSrcBand1 = GDALGetRasterBand(hSrcDataset, 1);
        if (!hSrcBand1)
            throw GDALException("Cannot read band 1 from " + input);

        // An optimized COG (Web Mercator, tiles, overviews) is encoded again at its own grid: GDAL
        // decodes every pixel and writes it again, so the output is what GDAL wrote here, never a
        // copy of the bytes of a file of a user, and a COG that is not on the tile grid keeps its
        // pixels instead of being resampled to it. Any other raster is warped to the tile grid.
        const bool ownGrid = isOptimizedCogDataset(hSrcDataset);

        LOGD << "COG " << (ownGrid ? "encoding at its own grid" : "warp") << " and compression of " << input
             << ": " << numThreads;

        // Scope GDAL's temporary directory (CPL_TMPDIR) to the output's own folder (the
        // per-build temp folder when called from the build pipeline). The COG driver
        // writes its overview/intermediate scratch into the directory of the final file
        // only when CPL_TMPDIR is unset; the cog.tif_<pid>_<counter> intermediates come
        // from CPLGenerateTempFilename(), which defaults to the process working directory.
        // On the Registry containers that working directory is an anonymous Docker volume
        // on the host root filesystem, so multi-GB COG scratch files accumulated on root.
        // Here they stay on the (large) datasets volume and are removed together with the
        // build temp folder. Thread-local: safe under concurrent builds. Not set for a bare
        // output filename (e.g. some CLI invocations).
        const std::string outputFolder = io::toUtf8(io::fromUtf8(outputCog).parent_path());
        std::optional<ScopedThreadConfigOption> gdalTmpGuard;
        if (!outputFolder.empty())
            gdalTmpGuard.emplace("CPL_TMPDIR", outputFolder.c_str());

        // Set the GDAL block cache to exactly the budget for the rest of the build:
        // raised on small hosts (fewer tile re-reads), lowered on large ones where
        // GDAL's 5%-of-RAM default made one warp peak at 12 GB without being faster.
        const ScopedGdalCacheMax cacheGuard(budget.cacheBytes);

        // Progress for diagnostic logging during long warp and encoding operations; they stop
        // when the build is canceled
        std::atomic<int> warpPct{0};
        cancel::GdalProgress progress([&warpPct, &reportProgress](double fraction) {
            reportWarpProgress(warpPct, fraction, reportProgress);
        });

        // Capture the OS write error immediately, before cleanup or logging can
        // replace it. Clearing errno before each write API avoids stale errors.
        GDALDatasetUniquePtr newDataset;
        int writeSystemError = 0;
        if (ownGrid) {
            CPLStringList targs = cogOwnGridArgs(hSrcDataset, hSrcBand1, numThreads);
            const GdalOptionsPtr<GDALTranslateOptions> psOptions(GDALTranslateOptionsNew(targs.List(), nullptr));
            if (!psOptions) throw GDALException("GDALTranslateOptionsNew returned null");
            progress.attach(psOptions.get());

            errno = 0;
            newDataset = adoptDataset(GDALTranslate(outputCog.c_str(), hSrcDataset, psOptions.get(), nullptr));
            writeSystemError = errno;
        } else {
            CPLStringList targs = cogWarpArgs(hSrcDataset, hSrcBand1, budget, numThreads, !ownDecodeThreads);
            const GdalOptionsPtr<GDALWarpAppOptions> psOptions(GDALWarpAppOptionsNew(targs.List(), nullptr));
            if (!psOptions) throw GDALException("GDALWarpAppOptionsNew returned null");
            progress.attach(psOptions.get());

            errno = 0;
            newDataset = adoptDataset(GDALWarp(outputCog.c_str(), nullptr, 1, &hSrcDataset, psOptions.get(), nullptr));
            writeSystemError = errno;
        }

        const std::string utility = ownGrid ? "GDALTranslate" : "GDALWarp";
        if (!newDataset) {
            cancel::throwIfCanceled();
            if (writeSystemError == ENOSPC)
                throw RetryableBuildException(utility + " ran out of space creating COG: " + outputCog,
                                              RetryableBuildReason::NoSpaceOnDevice);
            throw GDALException(utility + " failed to create output COG: " + outputCog);
        }
        const auto closed = closeOutput(std::move(newDataset));
        srcDataset.reset();
        if (closed.failed) {
            if (closed.systemError == ENOSPC)
                throw RetryableBuildException("COG close ran out of space: " + outputCog,
                                              RetryableBuildReason::NoSpaceOnDevice);
            throw GDALException("Failed to close output COG: " + outputCog);
        }

        // Generate stats.json sidecar
        generateCogStats(outputCog);
    }

    } // namespace raster

    namespace
    {
        // The private folder beside the output that the helper writes a COG and its statistics in
        constexpr const char *kCogStagingPrefix = ".cog-stage-";

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

    void buildCog(const std::string &input, const std::string &outputCog, const BuildOptions &options)
    {
        const fs::path target = fs::absolute(io::fromUtf8(outputCog));
        if (!target.has_filename())
            throw InvalidArgsException("Not the path of a file for a COG: " + outputCog);

        // The folder of the output holds other files (programs, the files of a user): the helper
        // writes into a folder of its own in it, which is removed with whatever is left in it
        const io::ScopedTempDir staging =
            raster::makePrivateFolder(target.parent_path(), kCogStagingPrefix);
        const fs::path produced = staging.path() / target.filename();
        buildCogInPrivateFolder(input, produced, options);

        // The COG first: the statistics count only while they are not older than it
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

        std::atomic<int> lastPct{0};
        const raster::ProgressCallback logProgress = [&lastPct](double fraction) {
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

    void generateCogStats(const std::string &cogPath) {
        auto dataset = raster::reopenWrittenRaster(io::fromUtf8(cogPath));
        if (!dataset) {
            LOGW << "Cannot open COG for stats generation: " << cogPath;
            return;
        }
        GDALDatasetH hDs = datasetHandle(dataset);

        json statsJson;
        json bandsJson;
        int nBands = GDALGetRasterCount(hDs);

        for (int i = 1; i <= nBands; i++) {
            GDALRasterBandH hBand = GDALGetRasterBand(hDs, i);
            double bMin, bMax, bMean, bStdDev;

            if (GDALComputeRasterStatistics(hBand, TRUE, &bMin, &bMax, &bMean, &bStdDev, nullptr, nullptr) != CE_None) {
                LOGW << "Cannot compute statistics for band " << i;
                continue;
            }

            // Approximate percentiles from histogram
            double p2 = bMin, p98 = bMax;
            int nBuckets = 256;
            GUIntBig *histogramRaw = nullptr;
            double hMin = bMin, hMax = bMax;
            const CPLErr histErr = GDALGetDefaultHistogramEx(hBand, &hMin, &hMax, &nBuckets,
                                                             &histogramRaw, FALSE, nullptr, nullptr);
            const VsiFreePtr<GUIntBig[]> histogram(histogramRaw);

            if (histErr == CE_None && histogram) {
                GUIntBig totalPixels = 0;
                for (int b = 0; b < nBuckets; b++) totalPixels += histogram[b];

                GUIntBig target2 = static_cast<GUIntBig>(totalPixels * 0.02);
                GUIntBig target98 = static_cast<GUIntBig>(totalPixels * 0.98);
                GUIntBig cumulative = 0;
                double bucketWidth = (hMax - hMin) / nBuckets;

                for (int b = 0; b < nBuckets; b++) {
                    cumulative += histogram[b];
                    if (cumulative >= target2 && p2 == bMin) {
                        p2 = hMin + b * bucketWidth;
                    }
                    if (cumulative >= target98 && p98 == bMax) {
                        p98 = hMin + b * bucketWidth;
                        break;
                    }
                }
            }

            bandsJson[std::to_string(i)] = {
                {"min", bMin}, {"max", bMax}, {"mean", bMean}, {"std", bStdDev},
                {"p2", p2}, {"p98", p98}
            };
        }

        dataset.reset();

        if (!bandsJson.empty()) {
            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);
            std::tm tmBuf;
#ifdef _WIN32
            gmtime_s(&tmBuf, &time_t);
#else
            gmtime_r(&time_t, &tmBuf);
#endif
            std::ostringstream oss;
            oss << std::put_time(&tmBuf, "%FT%TZ");

            statsJson["bands"] = bandsJson;
            statsJson["computedAt"] = oss.str();

            const fs::path statsPath = statsSidecarOf(io::fromUtf8(cogPath));
            std::ofstream out(statsPath);
            if (out.is_open()) {
                out << statsJson.dump(2);
                LOGD << "Wrote stats sidecar: " << io::toUtf8(statsPath);
            } else {
                LOGW << "Cannot write stats sidecar: " << io::toUtf8(statsPath);
            }
        }
    }

}
