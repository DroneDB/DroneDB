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

// The nodata value of one band: `has` tells whether the band declares one
struct BandNodata {
    bool has = false;
    double value = 0.0;
};

// Reads the nodata value of a band. The one place that calls GDALGetRasterNoDataValue.
DDB_DLL BandNodata readBandNodata(GDALRasterBandH band);

// The one rule that says whether a sample is the band's nodata value: an exact match in the
// precision of the sample, GDAL's own rule for its nodata masks. A NaN nodata value matches NaN
// samples and nothing else; a band that declares none has no nodata sample. A sample that is
// NaN or infinite without matching is not nodata here: callers that exclude every non-finite
// sample test std::isfinite as well.
//
// float samples (what the analyses read, whatever the band's type) are compared with the
// nodata value rounded to float, so -9999.9 declared by a Float64 band matches the float32
// sample read from it. double samples are compared as they are.
inline bool isNodata(float v, const BandNodata& nd) {
    if (!nd.has) return false;
    if (std::isnan(nd.value)) return std::isnan(v);
    return v == static_cast<float>(nd.value);
}

inline bool isNodata(double v, const BandNodata& nd) {
    if (!nd.has) return false;
    if (std::isnan(nd.value)) return std::isnan(v);
    return v == nd.value;
}

struct RasterBandNodata {
    int alphaBandIdx = -1;
    std::vector<BandNodata> bands;  // one per band, in band order
};

// Detect alpha band and nodata values for each band of a GDAL dataset
DDB_DLL RasterBandNodata detectBandNodata(GDALDatasetH hDataset, int bandCount);

// Pre-mask pixels: sets all bands to NODATA_SENTINEL where alpha==0
// or any band matches its GDAL nodata value (NaN-safe comparison).
// A null entry of bandPtrs is a band that was not read: it is neither inspected nor
// written, so the caller decides which bands decide the mask (the alpha band, if it
// is null, does not mask).
DDB_DLL void premaskNodata(const std::vector<float*>& bandPtrs,
                           size_t pixelCount,
                           int bandCount,
                           const RasterBandNodata& info,
                           float outputNodata = NODATA_SENTINEL);

}  // namespace ddb

#endif  // RASTER_UTILS_H
