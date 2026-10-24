/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef RASTER_UTILS_H
#define RASTER_UTILS_H

#include <vector>
#include <cmath>
#include "gdal_inc.h"

namespace ddb {

constexpr float NODATA_SENTINEL = -9999.0f;

// The nodata value of one band: `has` tells whether the band declares one
struct BandNodata {
    bool has = false;
    double value = 0.0;
};

// Reads the geotransform of a dataset into gt. Throws GDALException("Raster has no geotransform:
// <the dataset's name>") when the dataset has none (GDAL would hand out its default one).
void requireGeoTransform(GDALDatasetH ds, double gt[6]);

// Reads the nodata value of a band. The one place that calls GDALGetRasterNoDataValue.
BandNodata readBandNodata(GDALRasterBandH band);

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
RasterBandNodata detectBandNodata(GDALDatasetH hDataset, int bandCount);

// The value of an alpha band that is opaque, derived from the band's data. GDAL assumes the
// maximum of the band's type (or its NBITS), which is wrong for the rasters that write 255 in a
// UInt16 alpha band (ODM). The approximate maximum of the band (GDALComputeRasterMinMax, from
// the overviews or a sample of the blocks, never persisted) is rounded up to the all-ones value
// that holds it, as an alpha band is a k-bit channel: 255 -> 255, 254 -> 255, 1 -> 1, 65535 ->
// 65535; a band with no value above 0 (all transparent as far as sampled, or no sample) has the
// maximum of its type (1 for floating-point types).
double alphaOpaqueValue(GDALRasterBandH alphaBand);

// Pre-mask pixels: sets all bands to NODATA_SENTINEL where alpha==0
// or any band matches its GDAL nodata value (NaN-safe comparison).
// A null entry of bandPtrs is a band that was not read: it is neither inspected nor
// written, so the caller decides which bands decide the mask (the alpha band, if it
// is null, does not mask).
void premaskNodata(const std::vector<float*>& bandPtrs,
                   size_t pixelCount,
                   int bandCount,
                   const RasterBandNodata& info,
                   float outputNodata = NODATA_SENTINEL);

}  // namespace ddb

#endif  // RASTER_UTILS_H
