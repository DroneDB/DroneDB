/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "gdal_inc.h"
#include "cog.h"
#include "cog_utils.h"
#include "logger.h"
#include "mio.h"
#include "exceptions.h"
#include "json.h"

#include <fstream>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace ddb
{
    void buildCog(const std::string &inputGTiff, const std::string &outputCog)
    {
        // Check if input is already an optimized COG
        if (isOptimizedCog(inputGTiff)) {
            LOGD << "Input file " << inputGTiff << " is already an optimized COG, copying instead of rebuilding";

            // Simply copy the file instead of rebuilding
            try {
                std::ifstream src(inputGTiff, std::ios::binary);
                std::ofstream dst(outputCog, std::ios::binary);

                if (!src.is_open()) {
                    throw AppException("Cannot open source file for reading: " + inputGTiff);
                }
                if (!dst.is_open()) {
                    throw AppException("Cannot open destination file for writing: " + outputCog);
                }

                dst << src.rdbuf();

                if (src.bad() || dst.bad()) {
                    throw AppException("Error occurred during file copy");
                }

                LOGD << "Successfully copied optimized COG from " << inputGTiff << " to " << outputCog;
                generateCogStats(outputCog);
                return;
            } catch (const std::exception& e) {
                LOGW << "Failed to copy COG file: " << e.what() << ". Falling back to rebuild.";
                // Fall through to normal rebuild process
            }
        }

        // Normal rebuild process for files that need optimization
        LOGD << "Building COG from " << inputGTiff << " (requires optimization)";

        GDALDatasetH hSrcDataset = GDALOpen(inputGTiff.c_str(), GA_ReadOnly);

        if (!hSrcDataset)
            throw GDALException("Cannot open " + inputGTiff + " for reading");

        // Check that the source has a valid CRS
        const char* srcProjRef = GDALGetProjectionRef(hSrcDataset);
        if (!srcProjRef || strlen(srcProjRef) == 0) {
            GDALClose(hSrcDataset);
            throw GDALException("Cannot build COG: input file has no coordinate reference system: " + inputGTiff);
        }

        // Detect and preserve nodata from source
        int hasNoData;
        double srcNoData = GDALGetRasterNoDataValue(GDALGetRasterBand(hSrcDataset, 1), &hasNoData);

        char **targs = nullptr;
        targs = CSLAddString(targs, "-of");
        targs = CSLAddString(targs, "COG");
        targs = CSLAddString(targs, "-t_srs");
        targs = CSLAddString(targs, "EPSG:3857");
        targs = CSLAddString(targs, "-multi");
        targs = CSLAddString(targs, "-wo");
        targs = CSLAddString(targs, "NUM_THREADS=ALL_CPUS");
        targs = CSLAddString(targs, "-co");
        targs = CSLAddString(targs, "NUM_THREADS=ALL_CPUS");
        targs = CSLAddString(targs, "-r");
        targs = CSLAddString(targs, "bilinear");
        targs = CSLAddString(targs, "-co");
        targs = CSLAddString(targs, "TILING_SCHEME=GoogleMapsCompatible");
        targs = CSLAddString(targs, "-co");
        targs = CSLAddString(targs, "PREDICTOR=YES");

        // Preserve nodata values for transparency
        if (hasNoData) {
            // Skip nodata preservation for special float values that cannot
            // be reliably serialized (infinity, NaN, extreme floats)
            if (std::isfinite(srcNoData) && std::fabs(srcNoData) < 1e+30) {
                targs = CSLAddString(targs, "-wo");
                targs = CSLAddString(targs, "UNIFIED_SRC_NODATA=YES");
                targs = CSLAddString(targs, "-dstnodata");
                std::ostringstream oss;
                oss << std::setprecision(15) << srcNoData;
                targs = CSLAddString(targs, oss.str().c_str());
            } else {
                LOGW << "Skipping nodata preservation for unsupported value: " << srcNoData;
            }
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
            if (all8Bit)
            {
                targs = CSLAddString(targs, "-co");
                targs = CSLAddString(targs, "COMPRESS=JPEG");
                targs = CSLAddString(targs, "-co");
                targs = CSLAddString(targs, "QUALITY=90");
            }
            else
            {
                // LZW by default for non-8bit data
                targs = CSLAddString(targs, "-co");
                targs = CSLAddString(targs, "COMPRESS=LZW");
            }
        }
        else
        {
            // LZW by default for data with nodata values or other band counts
            targs = CSLAddString(targs, "-co");
            targs = CSLAddString(targs, "COMPRESS=LZW");
        }

        // BigTIFF
        targs = CSLAddString(targs, "-co");
        targs = CSLAddString(targs, "BIGTIFF=IF_SAFER");

        GDALWarpAppOptions *psOptions = GDALWarpAppOptionsNew(targs, nullptr);
        CSLDestroy(targs);
        GDALDatasetH hNewDataset = GDALWarp(outputCog.c_str(),
                                            nullptr,
                                            1,
                                            &hSrcDataset,
                                            psOptions,
                                            nullptr);
        GDALWarpAppOptionsFree(psOptions);
        if (!hNewDataset) {
            GDALClose(hSrcDataset);
            throw GDALException("GDALWarp failed to create output COG: " + outputCog);
        }
        GDALClose(hNewDataset);
        GDALClose(hSrcDataset);

        // Generate stats.json sidecar
        generateCogStats(outputCog);
    }

    void generateCogStats(const std::string &cogPath) {
        GDALDatasetH hDs = GDALOpen(cogPath.c_str(), GA_ReadOnly);
        if (!hDs) {
            LOGW << "Cannot open COG for stats generation: " << cogPath;
            return;
        }

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
            GUIntBig *histogram = nullptr;
            double hMin = bMin, hMax = bMax;

            if (GDALGetDefaultHistogramEx(hBand, &hMin, &hMax, &nBuckets, &histogram, FALSE, nullptr, nullptr) == CE_None && histogram) {
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
                VSIFree(histogram);
            }

            bandsJson[std::to_string(i)] = {
                {"min", bMin}, {"max", bMax}, {"mean", bMean}, {"std", bStdDev},
                {"p2", p2}, {"p98", p98}
            };
        }

        GDALClose(hDs);

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
