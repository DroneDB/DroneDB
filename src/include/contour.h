/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CONTOUR_H
#define CONTOUR_H

#include <optional>
#include <string>
#include "ddb_export.h"

namespace ddb {

/**
 * Options for contour line generation.
 *
 * Either `interval` or `count` must be provided. When both are set,
 * `interval` wins. If only `count` is provided, the interval is derived
 * from the raster band statistics as (max - min) / count.
 */
struct ContourOptions {
    std::optional<double> interval;     // Vertical interval between contours
    std::optional<int>    count;        // Target number of contour levels
    double                baseOffset = 0.0;     // Reference base elevation
    std::optional<double> minElev;      // Clip levels below this value
    std::optional<double> maxElev;      // Clip levels above this value
    double                simplifyTolerance = 0.0; // Geometry simplification (in raster CRS units, 0 = none)
    int                   bandIndex = 1;          // 1-based raster band index
};

/**
 * Generate contour lines from a single-band raster (DEM/DSM/DTM).
 *
 * Output JSON schema:
 * {
 *   "type": "FeatureCollection",
 *   "features": [
 *     {
 *       "type": "Feature",
 *       "geometry": { "type": "LineString" | "MultiLineString",
 *                     "coordinates": [...] },         // WGS84 lon/lat
 *       "properties": { "elev": <double> }
 *     }, ...
 *   ],
 *   "interval": <double>,
 *   "baseOffset": <double>,
 *   "min": <double>,
 *   "max": <double>,
 *   "featureCount": <int>,
 *   "unit": <string>
 * }
 *
 * @throws InvalidArgsException when neither interval nor count is supplied,
 *         when the raster cannot be read, or when bandIndex is invalid.
 * @throws GDALException when GDAL fails to compute contours.
 */
DDB_DLL std::string generateContoursJson(const std::string &rasterPath,
                                         const ContourOptions &options);

} // namespace ddb

#endif // CONTOUR_H
