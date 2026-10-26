/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "gdal_raii.h"
#include "cog_utils.h"
#include "logger.h"
#include "exceptions.h"

namespace ddb
{
    bool decodesWithOwnThreads(const std::string &driverName)
    {
        return driverName == "JP2OpenJPEG";
    }

    bool isOptimizedCog(const std::string &inputPath)
    {
        const auto dataset = adoptDataset(GDALOpen(inputPath.c_str(), GA_ReadOnly));
        if (!dataset) {
            LOGD << "Cannot open " << inputPath << " for COG verification";
            return false;
        }
        GDALDatasetH hDataset = datasetHandle(dataset);

        bool isOptimized = false;

        try {
            // Check if it's already in Web Mercator (EPSG:3857)
            const char* projRef = GDALGetProjectionRef(hDataset);
            if (!projRef || strlen(projRef) == 0) {
                LOGD << "File has no projection, needs rebuild";
                return false;
            }

            const SrsPtr hSRS(OSRNewSpatialReference(projRef));
            if (!hSRS) {
                LOGD << "Cannot parse projection, needs rebuild";
                return false;
            }

            // Check if it's Web Mercator (EPSG:3857)
            const SrsPtr hTargetSRS(OSRNewSpatialReference(nullptr));
            if (OSRImportFromEPSG(hTargetSRS.get(), 3857) != OGRERR_NONE) {
                LOGD << "Cannot import EPSG:3857, needs rebuild";
                return false;
            }

            if (!OSRIsSame(hSRS.get(), hTargetSRS.get())) {
                LOGD << "File not in EPSG:3857, needs reprojection";
                return false;
            }

            // Check if it has proper tiling
            int blockXSize, blockYSize;
            GDALRasterBandH hBand = GDALGetRasterBand(hDataset, 1);
            GDALGetBlockSize(hBand, &blockXSize, &blockYSize);

            // COG should have blocks that are powers of 2, typically 256x256 or 512x512
            bool properTiling = (blockXSize == blockYSize) &&
                               (blockXSize == 256 || blockXSize == 512);

            if (!properTiling) {
                LOGD << "File doesn't have proper tiling (" << blockXSize << "x" << blockYSize << "), needs rebuild";
                return false;
            }

            // Check if it has overviews
            int overviewCount = GDALGetOverviewCount(hBand);
            if (overviewCount == 0) {
                LOGD << "File has no overviews, needs rebuild";
                return false;
            }

            // Check if driver is COG (if GDAL supports it)
            GDALDriverH hDriver = GDALGetDatasetDriver(hDataset);
            const char* driverName = GDALGetDriverShortName(hDriver);
            bool isCOGDriver = (strcmp(driverName, "COG") == 0 || strcmp(driverName, "GTiff") == 0);

            if (!isCOGDriver) {
                LOGD << "File driver is not COG compatible (" << driverName << "), needs rebuild";
                return false;
            }

            LOGD << "File appears to be an optimized COG with " << overviewCount << " overviews";
            isOptimized = true;

        } catch (const std::exception &e) {
            LOGD << "Error checking COG status (" << e.what() << "), assuming needs rebuild";
            isOptimized = false;
        }

        return isOptimized;
    }

}