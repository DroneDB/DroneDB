/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef RASTER_UTILS_H
#define RASTER_UTILS_H

#include <vector>
#include <cmath>
#include "gdal_inc.h"
#include "ddb_export.h"

namespace ddb {

constexpr float NODATA_SENTINEL = -9999.0f;

struct RasterBandNodata {
    int alphaBandIdx = -1;
    std::vector<int> hasNodata;
    std::vector<double> nodataValues;
};

// Detect alpha band and nodata values for each band of a GDAL dataset
DDB_DLL RasterBandNodata detectBandNodata(GDALDatasetH hDataset, int bandCount);

// Pre-mask pixels: sets all bands to NODATA_SENTINEL where alpha==0
// or any band matches its GDAL nodata value (NaN-safe comparison)
DDB_DLL void premaskNodata(const std::vector<float*>& bandPtrs,
                           size_t pixelCount,
                           int bandCount,
                           const RasterBandNodata& info,
                           float outputNodata = NODATA_SENTINEL);

}  // namespace ddb

#endif  // RASTER_UTILS_H
