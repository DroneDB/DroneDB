/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef RASTER_ANALYSIS_H
#define RASTER_ANALYSIS_H

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ddb {

/**
 * Get info about a single-band raster used for analysis (thermal image,
 * DEM/DSM/DTM, or any generic value raster).
 *
 * Output JSON schema (neutral field names):
 * {
 *   "width": int, "height": int, "bandCount": int,
 *   "dataType": "Float32"|"UInt16"|...,
 *   "valueMin": float, "valueMax": float,
 *   "unit": string          // from GDAL UNITTYPE, "" if unknown; "\u00B0C" if thermal
 *   "isThermal": bool,
 *   "isDirectValue": bool,   // true when values are already in target units
 *   "sensorId": string,      // detected sensor profile id (if any)
 *   "calibration": {...}     // only when isThermal && calibration valid
 * }
 */
std::string getRasterValueInfoJson(const std::string &filePath);

/**
 * Get raster value at pixel (x, y).
 *
 * Output JSON schema:
 * {
 *   "value": float,
 *   "rawValue": float,
 *   "x": int, "y": int,
 *   "geoX": float, "geoY": float, "hasGeo": bool,
 *   "isThermal": bool
 * }
 */
std::string getRasterPointJson(const std::string &filePath, int x, int y);

/**
 * Get statistics over pixel rectangle [x0,y0]-[x1,y1] (inclusive).
 *
 * Output JSON schema:
 * {
 *   "min": float, "max": float, "mean": float, "stddev": float, "median": float,
 *   "pixelCount": int,
 *   "bounds": { "x0": int, "y0": int, "x1": int, "y1": int },
 *   "unit": string,
 *   "isThermal": bool
 * }
 */
std::string getRasterAreaStatsJson(const std::string &filePath,
                                   int x0, int y0, int x1, int y1);

/**
 * The metadata of a raster for a viewer (DDBGetRasterMetadata): statistics, the formulas it
 * can compute, the colormaps and the detected sensor.
 *
 * Output JSON schema (tests/contract/schemas/raster-metadata.schema.json):
 * {
 *   "statistics": {"<band>": {"min", "max", "mean", "std", "percentiles": {"p2", "p98"},
 *                              "histogram": {"counts", "edges", "bins", "below", "above"}  // formula only
 *                            }} | null,
 *   "algorithms": [{"id", "name", "expr", "help", "range"?, "disabled"?, "disabledReason"?}],
 *   "colormaps": ..., "autoBands": {"filter", "match"}, "detectedSensor": string
 * }
 * @param formula "" for the statistics of each band, else the id of a formula: its statistics
 *                and histogram over the raster sampled at 512 x 512 at most, as band "1"; the
 *                histogram bins formulaHistogramRange, "below" and "above" count the values
 *                outside its edges (min and max are those of all the values)
 * @param bandFilter "" to detect the bands from the sensor profile of the raster
 */
std::string getRasterMetadataJson(const std::string &path, const std::string &formula, const std::string &bandFilter);

/**
 * A histogram of bins of equal width between edges.front() and edges.back(): counts[i] holds
 * the values in [edges[i], edges[i + 1]) (the last bin also holds edges.back()); below and
 * above count the values outside the edges.
 */
struct ValueHistogram {
    std::vector<int> counts;
    std::vector<double> edges;  // counts.size() + 1
    int below = 0;
    int above = 0;
};

/**
 * The range the histogram of a formula's values bins: [min, max] narrowed to the formula's
 * valid range (when it has one) and to one 2%-98% spread around the 2nd and 98th percentiles
 * ([p2 - (p98 - p2), p98 + (p98 - p2)]), so a few outliers (an NDVI of -191 where the ground has
 * no signal) do not crush the other values into a bar or two. [min, max] when the narrowed range
 * is empty; [min, min + 1] when all the values are one.
 */
std::pair<double, double> formulaHistogramRange(double min, double max, double p2, double p98,
                                                const std::optional<std::pair<double, double>> &validRange);

/** The histogram of values in bins of equal width over [lo, hi] (hi > lo, bins > 0). */
ValueHistogram binValues(const std::vector<double> &values, double lo, double hi, int bins);

/**
 * The sensor profile info of a raster (SensorProfileManager::getRasterInfoJson) with "indexLayer"
 * added: the indices the WMS vegetation-index layer can render from it (DDBGetRasterInfo).
 */
std::string getRasterInfoJson(const std::string &path);

} // namespace ddb

#endif // RASTER_ANALYSIS_H
