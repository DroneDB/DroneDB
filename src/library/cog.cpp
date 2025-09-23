/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "gdal_inc.h"
#include "cog.h"
#include "cog_utils.h"
#include "logger.h"
#include "mio.h"
#include "exceptions.h"

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
            targs = CSLAddString(targs, "-wo");
            targs = CSLAddString(targs, "UNIFIED_SRC_NODATA=YES");
            targs = CSLAddString(targs, "-dstnodata");
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(0) << srcNoData;
            targs = CSLAddString(targs, oss.str().c_str());
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
        GDALClose(hNewDataset);
        GDALClose(hSrcDataset);
    }

}
