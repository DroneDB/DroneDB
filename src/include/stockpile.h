/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef STOCKPILE_H
#define STOCKPILE_H

#include <string>
#include "ddb_export.h"

namespace ddb {

/**
 * Auto-detect a stockpile footprint by clicking on an elevation raster.
 *
 * Pipeline: local window -> border-based base plane -> gaussian smoothing ->
 * adaptive threshold -> 4-connected flood fill from the click point ->
 * Moore-neighbour contour tracing -> Douglas-Peucker simplification ->
 * GeoJSON polygon in WGS84.
 *
 * Output JSON schema:
 * {
 *   "polygon":         GeoJSON Polygon (WGS84),
 *   "estimatedVolume": m^3,
 *   "confidence":      float in [0, 1],
 *   "baseElevation":   m,
 *   "basePlaneMethod": "auto",
 *   "searchRadius":    m,
 *   "sensitivityUsed": float in [0, 1],
 *   "pixelCount":      int
 * }
 *
 * @param rasterPath   Path to a single-band DEM (GeoTIFF, COG, VRT...)
 * @param lat          Click latitude (WGS84, degrees)
 * @param lon          Click longitude (WGS84, degrees)
 * @param radiusMeters Search radius around the click (meters, >0)
 * @param sensitivity  [0,1] - higher = finer detail, lower = smoother boundary
 *
 * Throws InvalidArgsException for bad user input,
 * AppException for GDAL/IO errors or when no stockpile is found.
 */
DDB_DLL std::string detectStockpileJson(const std::string &rasterPath,
                                        double lat, double lon,
                                        double radiusMeters,
                                        float sensitivity);

/**
 * Auto-detect ALL stockpile footprints by full-DEM scan.
 *
 * Pipeline: full raster -> low-pass base plane -> diff -> gaussian smoothing
 * -> adaptive global threshold -> two-pass connected components labeling
 * -> per-component contour tracing + Douglas-Peucker simplification
 * -> sort by estimated volume desc -> truncate to maxResults.
 *
 * Output JSON schema:
 * {
 *   "stockpiles": [
 *     {
 *       "id": int,
 *       "polygon": GeoJSON Polygon (WGS84),
 *       "estimatedVolume": m^3,
 *       "confidence": float in [0, 1],
 *       "baseElevation": m,
 *       "centroid": [lon, lat],
 *       "areaM2": m^2,
 *       "pixelCount": int
 *     }, ...
 *   ],
 *   "totalFound": int,
 *   "sensitivityUsed": float,
 *   "minAreaUsed": m^2
 * }
 *
 * @param rasterPath  Path to single-band DEM
 * @param sensitivity [0,1] - higher = finer detail
 * @param minAreaM2   Minimum component area in square meters (>=0)
 * @param maxResults  Maximum number of stockpiles returned (>0, server-capped to 500)
 */
DDB_DLL std::string detectAllStockpilesJson(const std::string &rasterPath,
                                            float sensitivity,
                                            double minAreaM2,
                                            int maxResults);

} // namespace ddb

#endif // STOCKPILE_H
