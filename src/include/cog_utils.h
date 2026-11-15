/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef COG_UTILS_H
#define COG_UTILS_H

#include <string>

namespace ddb {

// Whether a raster is already an optimized COG, which a COG build encodes again at its own grid
// without a warp: in Web Mercator, tiled in square blocks of 256 or 512 pixels, with overviews (the
// rule of ddb-tilers' builder, ddbtIsOptimizedCog). False for a file that cannot be read. A file of a
// user: only in the raster helper (std::logic_error elsewhere, see gdal_open_policy.h).
bool isOptimizedCog(const std::string& inputPath);

// The fraction of a COG build from which its phase is "statistics", up to 1. ddb-tilers computes the
// exact statistics of the bands in the same pass that warps, reduces and encodes the raster, so the
// phases are spans of the fraction of that one pass, not steps of their own; this one is kept at
// half, the share that the separate pass over the COG written had of a build before (40 to 55% of a
// 3-band orthophoto and of a 52-band Float32 raster), so that the phases reported do not change.
inline constexpr double kCogStatisticsStart = 0.5;

// The phase of a COG build that has done `fraction` (0 to 1) of its work: "warping" (the raster is
// reprojected to the tile grid), "overviews" (the pyramid is made), "compressing" (the tiles are encoded
// and written) and "statistics" (the exact statistics of the bands), the spans of kCogStatisticsStart.
// The first three share the fractions below kCogStatisticsStart: with a warp they end at 3/8 and 1/2
// of it (0.1875 and 0.25). An optimized COG is encoded again at its own grid without a warp (hasWarp
// false): its overviews end at 0.1 and it is never "warping". The statistics are from kCogStatisticsStart (0.5) to 1.
const char* cogPhaseOfFraction(double fraction, bool hasWarp);

}

#endif  // COG_UTILS_H