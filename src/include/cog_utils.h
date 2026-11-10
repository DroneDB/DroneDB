/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef COG_UTILS_H
#define COG_UTILS_H

#include <string>

#include <gdal.h>

namespace ddb {

// Check if a GeoTIFF file is already an optimized Cloud Optimized GeoTIFF: Web Mercator, square
// tiles of 256 or 512 pixels, overviews, read by the GTiff or COG driver
bool isOptimizedCog(const std::string& inputPath);

// The same check on a dataset that is open already
bool isOptimizedCogDataset(GDALDatasetH hDataset);

// The fraction of a COG build at which the work of GDAL's COG driver ends and the exact statistics of
// its bands (generateCogStats) begin: the driver's fractions are reported scaled into [0, this), the
// statistics fill the rest, up to 1. Half: the pass over every pixel of every band of the COG written
// took 40 to 55% of a build (a 3-band orthophoto, a 52-band Float32 raster).
inline constexpr double kCogStatisticsStart = 0.5;

// The phase of a COG build that has done `fraction` (0 to 1) of its work: "warping" (the raster is
// reprojected to the tile grid), "overviews" (the pyramid is made), "compressing" (the tiles are encoded
// and written) and "statistics" (the exact statistics of the bands are computed from the COG written).
// The first three are those of GDAL's COG driver, whose work is in pixel units: the warp one unit per
// pixel and band, the overviews a third of that, the final write 4/3 of it, scaled into the fractions
// below kCogStatisticsStart. With a warp they end at 3/8 and 1/2 of that (0.1875 and 0.25). An
// optimized COG is encoded again at its own grid without a warp (hasWarp false; cog.cpp,
// cogOwnGridArgs): its overviews are 1/5 of the driver's work, so they end at 0.1 and it is never
// "warping". The statistics are from kCogStatisticsStart (0.5) to 1.
const char* cogPhaseOfFraction(double fraction, bool hasWarp);

}

#endif  // COG_UTILS_H