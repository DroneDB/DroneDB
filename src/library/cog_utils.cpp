/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "cog_utils.h"

#include "ddbt/ddbt.h"
#include "gdal_open_policy.h"
#include "logger.h"

namespace ddb
{
    bool isOptimizedCog(const std::string &inputPath)
    {
        // A file of a user is read in the raster helper only, as GDAL opens it there
        gdalopen::requireUserFileOpens("isOptimizedCog");
        bool optimized = false;
        if (ddbtIsOptimizedCog(inputPath.c_str(), &optimized) != DDBT_ERR_NONE)
        {
            LOGD << "Cannot check whether " << inputPath << " is an optimized COG: " << ddbtLastError();
            return false;
        }
        return optimized;
    }

    const char* cogPhaseOfFraction(double fraction, bool hasWarp)
    {
        // The spans below kCogStatisticsStart are the shares of the warp (1), the overviews (1/3) and the
        // final write (4/3, in units of the pixels of the raster) in the GDAL COG build that ddb-tilers
        // replaced, kept so that the phases reported do not change. Without the warp the overviews are 1/5
        // of them and no fraction is warping
        const double overviewsStart = hasWarp ? kCogStatisticsStart * 3.0 / 8.0 : 0.0;
        const double compressingStart = hasWarp ? kCogStatisticsStart / 2.0 : kCogStatisticsStart / 5.0;
        if (fraction < overviewsStart) return "warping";
        if (fraction < compressingStart) return "overviews";
        if (fraction < kCogStatisticsStart) return "compressing";
        return "statistics";
    }

}