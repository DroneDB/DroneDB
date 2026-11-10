/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CONTOUR_H
#define CONTOUR_H

#include <functional>
#include <optional>
#include <string>

namespace ddb {

// The most lines of one contour result (ContourOptions::maxFeatures by default)
constexpr int kMaxContourFeatures = 100000;

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
    std::optional<double> nodata;       // NoData value that replaces the one of the band (FR-RAS-31)
    std::string           formula;      // a thermal formula (CELSIUS, KELVIN, FAHRENHEIT): the levels, the options below and
                                        // the output are in its unit, converted from the unit of the band (D-022); "" = the band's.
                                        // Any other formula (an index) is ignored, as in 2.2; one that does not exist is refused
    // The most lines the output holds (kMaxContourFeatures): a result with more is cut there and says so
    // (truncated, featureCap). Lower only to test the cap; the C API leaves it as it is
    int                   maxFeatures = kMaxContourFeatures;
    // Called with the fraction done in [0,1] and a short phase label ("statistics", "levels", "lines"); returns
    // true to cancel the generation (it stops at its next checkpoint, as a canceled token does). New in 2.3
    std::function<bool(double fraction, const char *phase)> progress;
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
 *   "unit": <string>,
 *   "truncated": <bool>,      // true when the lines were cut at "featureCap" (new in 2.3)
 *   "featureCap": <int>       // the most lines a result holds (new in 2.3)
 * }
 *
 * @throws InvalidArgsException when neither interval nor count is supplied,
 *         when the raster cannot be read, or when bandIndex is invalid.
 * @throws GDALException when GDAL fails to compute contours.
 */
std::string generateContoursJson(const std::string &rasterPath,
                                 const ContourOptions &options);

} // namespace ddb

#endif // CONTOUR_H
