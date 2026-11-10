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

// The phase of a COG build that has done `fraction` (0 to 1) of its work: "warping" (the raster is
// reprojected to the tile grid), "overviews" (the pyramid is made) and "compressing" (the tiles are
// encoded and written). The fractions are those of GDAL's COG driver, whose work is in pixel units:
// the warp one unit per pixel and band, the overviews a third of that, the final write 4/3 of it. With a
// warp the phases end at 3/8 and 1/2 of the work. An optimized COG is encoded again at its own grid
// without a warp (hasWarp false; cog.cpp, cogOwnGridArgs): its overviews are 1/5 of the work, so they
// end at 1/5 and it is never "warping".
const char* cogPhaseOfFraction(double fraction, bool hasWarp);

}

#endif  // COG_UTILS_H