/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef VOLUME_H
#define VOLUME_H

#include <string>
#include "ddb_export.h"

namespace ddb {

/**
 * Stockpile volume calculation on a single-band elevation raster (DSM/DTM).
 *
 * Returns a JSON document with cut/fill volumes, 2D/3D area, the base plane
 * used for the computation, pixel size and CRS. The polygon geometry is
 * expected as GeoJSON in WGS84 (lon/lat); the function reprojects it into the
 * raster CRS internally.
 *
 * Supported base-plane methods (case-insensitive):
 *   - "lowest_perimeter"   (default)
 *   - "average_perimeter"
 *   - "best_fit"          (least-squares plane z = ax + by + c)
 *   - "flat"              (uses @p flatElevation)
 *
 * Output JSON schema:
 * {
 *   "cutVolume":       m^3,
 *   "fillVolume":      m^3,
 *   "netVolume":       m^3,   // cut - fill
 *   "area2d":          m^2,   // projected footprint
 *   "area3d":          m^2,   // surface area using local slope
 *   "baseElevation":   m,     // average plane elevation inside the polygon
 *   "basePlaneMethod": string,
 *   "pixelSize":       m,
 *   "crs":             string,
 *   "boundaryPolygon": GeoJSON,   // echo of the input polygon
 *   "pixelCount":      int,
 *   "calculatedAt":    ISO-8601
 * }
 *
 * Throws InvalidArgsException for bad input (geometry, method name),
 * AppException for runtime GDAL/IO errors.
 */
DDB_DLL std::string calculateVolumeJson(const std::string &rasterPath,
                                        const std::string &polygonGeoJson,
                                        const std::string &baseMethod,
                                        double flatElevation);

} // namespace ddb

#endif // VOLUME_H
