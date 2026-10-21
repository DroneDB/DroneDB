/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "gdal_raii.h"
#include "cog.h"
#include "cancellation.h"
#include "build_budget.h"
#include "parallel.h"
#include "cog_utils.h"
#include "logger.h"
#include "mio.h"
#include "exceptions.h"
#include "json.h"

#include <fstream>
#include <optional>
#include <cerrno>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace ddb
{
    // Logs warp progress every ~5% to aid diagnostics. Uses an atomic counter to throttle
    // log output (GDAL may call this very frequently, from its own threads too).
    static void logWarpProgress(std::atomic<int>& lastPct, double dfComplete) {
        const int pct = static_cast<int>(dfComplete * 100.0);
        int prev = lastPct.load();
        if (pct >= prev + 5 && lastPct.compare_exchange_strong(prev, pct)) {
            LOGD << "COG warp progress: " << pct << "%";
        }
    }

    void buildCog(const std::string &inputGTiff, const std::string &outputCog)
    {
        // Check if input is already an optimized COG
        if (isOptimizedCog(inputGTiff)) {
            LOGD << "Input file " << inputGTiff << " is already an optimized COG, copying instead of rebuilding";

            // Simply copy the file instead of rebuilding
            try {
                // The filesystem wrapper carries the actual write error code.
                // A full disk must escape the fallback and preserve pending work.
                io::copy(inputGTiff, outputCog);

                LOGD << "Successfully copied optimized COG from " << inputGTiff << " to " << outputCog;
                generateCogStats(outputCog);
                return;
            } catch (const FSException& e) {
                if (e.code() == std::errc::no_space_on_device)
                    throw;
                LOGW << "Failed to copy COG file: " << e.what() << ". Falling back to rebuild.";
                // Fall through to normal rebuild process
            } catch (const std::exception& e) {
                LOGW << "Failed to copy COG file: " << e.what() << ". Falling back to rebuild.";
                // Fall through to normal rebuild process
            }
        }

        // Normal rebuild process for files that need optimization
        LOGD << "Building COG from " << inputGTiff << " (requires optimization)";

        // Owned: the budget, the option list and GDALWarpAppOptionsNew below can throw
        auto srcDataset = adoptDataset(GDALOpen(inputGTiff.c_str(), GA_ReadOnly));

        if (!srcDataset)
            throw GDALException("Cannot open " + inputGTiff + " for reading");
        GDALDatasetH hSrcDataset = datasetHandle(srcDataset);

        // Check that the source has a valid CRS
        const char* srcProjRef = GDALGetProjectionRef(hSrcDataset);
        if (!srcProjRef || strlen(srcProjRef) == 0)
            throw GDALException("Cannot build COG: input file has no coordinate reference system: " + inputGTiff);

        // Detect and preserve nodata from source
        int hasNoData;
        GDALRasterBandH hSrcBand1 = GDALGetRasterBand(hSrcDataset, 1);
        if (!hSrcBand1)
            throw GDALException("Cannot read band 1 from " + inputGTiff);
        double srcNoData = GDALGetRasterNoDataValue(hSrcBand1, &hasNoData);

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

        // Threads, warp memory and block cache come from one budget (DDB-08) so
        // that several builds running in one process stay within known bounds;
        // the threads beyond this one take CPU tokens (DQ-15), held until the
        // output is closed.
        const BuildResourceBudget budget = BuildResourceBudget::fromConfig();
        const auto cpu = parallel::tryAcquireThreads(static_cast<size_t>(budget.threads));
        const std::string numThreads = parallel::gdalNumThreads(cpu);

        CPLStringList targs;
        targs.AddString("-of");
        targs.AddString("COG");
        targs.AddString("-t_srs");
        targs.AddString("EPSG:3857");
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

        // Preserve nodata values for transparency
        if (hasNoData) {
            // Skip nodata preservation for special float values that cannot
            // be reliably serialized (infinity, NaN, extreme floats)
            if (std::isfinite(srcNoData) && std::fabs(srcNoData) < 1e+30) {
                targs.AddString("-wo");
                targs.AddString("UNIFIED_SRC_NODATA=YES");
                targs.AddString("-dstnodata");
                std::ostringstream oss;
                oss << std::setprecision(15) << srcNoData;
                targs.AddString(oss.str().c_str());
            } else {
                // The nodata value cannot be reliably serialized as a
                // -dstnodata string (NaN/Inf/extreme float). Materialize the
                // transparency as a destination alpha band instead, so masked
                // pixels do not become opaque in the output COG. GDALWarp still
                // reads the source nodata from the band itself to drive the mask.
                LOGW << "Nodata value cannot be serialized (" << srcNoData
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

        // We can compress to JPG if these are 8bit bands (3 or 4) and no nodata
        const int numBands = GDALGetRasterCount(hSrcDataset);

        // We can compress to JPG if these are 8bit bands (3 or 4) and no nodata
        if ((numBands == 3 || numBands == 4) && !hasNoData)
        {
            bool all8Bit = true;

            for (int n = 0; n < numBands; n++)
            {
                GDALRasterBandH b = GDALGetRasterBand(hSrcDataset, n + 1);
                if (GDALGetRasterDataType(b) != GDT_Byte)
                {
                    all8Bit = false;
                }
            }
            // Only treat a 4-band raster as JPEG-eligible RGBA when the 4th band
            // is actually an alpha channel. A 4-band RGBN raster (e.g. with a NIR
            // band) must NOT be JPEG-compressed: the COG/JPEG path treats band 4
            // as alpha and would lossily corrupt the spectral data. Fall back to
            // LZW for such datasets.
            bool jpegEligible = all8Bit;
            if (numBands == 4 &&
                GDALGetRasterColorInterpretation(GDALGetRasterBand(hSrcDataset, 4)) != GCI_AlphaBand)
            {
                jpegEligible = false;
            }
            if (jpegEligible)
            {
                targs.AddString("-co");
                targs.AddString("COMPRESS=JPEG");
                // JPEG quality: configurable via DDB_COG_JPEG_QUALITY (default: 75, matching GDAL default).
                // PREDICTOR is not valid for JPEG and is intentionally omitted.
                int jpegQuality = 75;
                const char* envQuality = CPLGetConfigOption("DDB_COG_JPEG_QUALITY", nullptr);
                if (envQuality) {
                    jpegQuality = std::clamp(std::atoi(envQuality), 1, 100);
                }
                targs.AddString("-co");
                targs.AddString(("QUALITY=" + std::to_string(jpegQuality)).c_str());
            }
            else
            {
                // LZW by default for non-8bit data
                targs.AddString("-co");
                targs.AddString("COMPRESS=LZW");
                targs.AddString("-co");
                targs.AddString("PREDICTOR=YES");
            }
        }
        else
        {
            // LZW by default for data with nodata values or other band counts
            targs.AddString("-co");
            targs.AddString("COMPRESS=LZW");
            targs.AddString("-co");
            targs.AddString("PREDICTOR=YES");
        }

        // BigTIFF
        targs.AddString("-co");
        targs.AddString("BIGTIFF=IF_SAFER");

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
        const std::string outputFolder = fs::path(outputCog).parent_path().string();
        std::optional<ScopedThreadConfigOption> gdalTmpGuard;
        if (!outputFolder.empty())
            gdalTmpGuard.emplace("CPL_TMPDIR", outputFolder.c_str());

        const GdalOptionsPtr<GDALWarpAppOptions> psOptions(
            GDALWarpAppOptionsNew(targs.List(), nullptr));
        if (!psOptions) throw GDALException("GDALWarpAppOptionsNew returned null");

        // Set the GDAL block cache to exactly the budget for the rest of the build:
        // raised on small hosts (fewer tile re-reads), lowered on large ones where
        // GDAL's 5%-of-RAM default made one warp peak at 12 GB without being faster.
        const ScopedGdalCacheMax cacheGuard(budget.cacheBytes);

        // Progress for diagnostic logging during long warp operations; the warp stops when
        // the build is canceled (DQ-15)
        std::atomic<int> warpPct{0};
        cancel::GdalProgress progress([&warpPct](double fraction) { logWarpProgress(warpPct, fraction); });
        progress.attach(psOptions.get());

        // Capture the OS write error immediately, before cleanup or logging can
        // replace it. Clearing errno before each write API avoids stale errors.
        errno = 0;
        auto newDataset = adoptDataset(GDALWarp(outputCog.c_str(),
                                               nullptr,
                                               1,
                                               &hSrcDataset,
                                               psOptions.get(),
                                               nullptr));
        const int warpSystemError = errno;

        if (!newDataset) {
            cancel::throwIfCanceled();
            if (warpSystemError == ENOSPC)
                throw RetryableBuildException("GDALWarp ran out of space creating COG: " + outputCog,
                                              RetryableBuildReason::NoSpaceOnDevice);
            throw GDALException("GDALWarp failed to create output COG: " + outputCog);
        }
        CPLErrorReset();
        errno = 0;
        newDataset.reset();
        const int closeSystemError = errno;
        const bool closeFailed = CPLGetLastErrorType() >= CE_Failure;
        srcDataset.reset();
        if (closeFailed) {
            if (closeSystemError == ENOSPC)
                throw RetryableBuildException("COG close ran out of space: " + outputCog,
                                              RetryableBuildReason::NoSpaceOnDevice);
            throw GDALException("Failed to close output COG: " + outputCog);
        }

        // Generate stats.json sidecar
        generateCogStats(outputCog);
    }

    void generateCogStats(const std::string &cogPath) {
        auto dataset = adoptDataset(GDALOpen(cogPath.c_str(), GA_ReadOnly));
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

            fs::path cogFsPath(cogPath);
            fs::path statsPath = cogFsPath.parent_path() / (cogFsPath.filename().string() + ".stats.json");
            std::ofstream out(statsPath.string());
            if (out.is_open()) {
                out << statsJson.dump(2);
                LOGD << "Wrote stats sidecar: " << statsPath.string();
            } else {
                LOGW << "Cannot write stats sidecar: " << statsPath.string();
            }
        }
    }

}
