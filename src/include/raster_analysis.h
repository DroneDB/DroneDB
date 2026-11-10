/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef RASTER_ANALYSIS_H
#define RASTER_ANALYSIS_H

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "thermal.h"

namespace ddb {

struct TemperatureData;
struct RawThermalData;
struct DemContext;

/**
 * The options that every raster value endpoint (value info, point, area statistics, profile, contours)
 * shares: one type, never a parameter per endpoint (FR-RAS-31, FR-XC-24, D-022).
 */
struct RasterValueOptions {
    /// NoData value that replaces the one of the band (FR-RAS-31); finite
    std::optional<double> nodata;
    /// The formula the values are read through: for the value info of a vegetation index, the index (its
    /// default stretch is that of the index, D-041). The thermal formulas (CELSIUS, KELVIN, FAHRENHEIT) name
    /// the unit of the values (D-022).
    std::string formula;
    /// Value info only: a WGS84 window (minLon, minLat, maxLon, maxLat); the info then reports the source
    /// pixels in it ("windowPixelCount")
    std::optional<std::array<double, 4>> bbox;
};

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
 *   "calibration": {...},    // only when isThermal && calibration valid (the Planck constants were read from the file):
 *                            // the Planck constants, and the measurement parameters the file says (never a default)
 *   "nodata": number|null,   // the NoData value that applies (the override, else the one of the band)
 *   "nodataOverridden": bool, "nodataInvalid": bool, // the NoData value cannot be held by the type of the band
 *   "scale", "offset"        // only when the band has them: valueMin/valueMax are physical values
 *   "pixelCount", "validPixelCount": int  // the whole raster, and its pixels that have a value
 *   "windowPixelCount": int  // only with options.bbox: the source pixels in the window
 *   "stretch": [{"min", "max"}]  // D-041: one per band, one for an index, [] for a Byte raster of several bands
 * }
 * valueMin and valueMax are those of the exact statistics (RasterStatsProvider) of the pixels that have a value.
 */
std::string getRasterValueInfoJson(const std::string &filePath, const RasterValueOptions &options = {});

/**
 * Get raster value at pixel (x, y).
 *
 * Output JSON schema:
 * {
 *   "value": float,          // in "unit"
 *   "rawValue": float,
 *   "unit": string,          // of the band ("\u00B0C" for a thermal raster that has none), or of the formula
 *   "x": int, "y": int,
 *   "geoX": float, "geoY": float, "hasGeo": bool,
 *   "isThermal": bool
 * }
 * With options: a pixel whose raw sample is the NoData value has no value (a NoRasterDataException), and a
 * thermal formula gives the value in its unit (D-022: CELSIUS in degrees Celsius, KELVIN in K, converted from
 * the unit of the band); any other formula is a RefusedArgumentException. The same for the area statistics.
 * A thermal image GDAL does not read (an R-JPEG) is read as its temperature grid (thermalGridPointJson), or, when
 * its raw counts have no conversion (DJI: DJI's own conversion is not available), as its raw counts without a unit;
 * a thermal formula on values that are no temperatures (raw counts without a calibration, the colour preview of a
 * picture) is a RefusedArgumentException. The value info of such an image says unit "", isDirectValue false and no
 * calibration, whatever the formula.
 */
std::string getRasterPointJson(const std::string &filePath, int x, int y, const RasterValueOptions &options = {});

/**
 * The raster value at a map coordinate (WGS84 lon/lat): the pixel under it, found with the one geo-to-pixel
 * helper (DemContext::wgs84ToPixelIndex) on this file. Same schema as getRasterPointJson, plus the point
 * asked for ("lon", "lat"). A point outside the raster, or on a pixel that has no value, is a
 * NoRasterDataException.
 */
std::string getRasterPointAtJson(const std::string &filePath, double lon, double lat,
                                 const RasterValueOptions &options = {});

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
 * The rectangle is read a window at a time, so that what the statistics hold does not grow with the area
 * (area_statistics_internal.h kMaxAreaBufferBytes): the values of up to 2^23 pixels are sorted, as they always were;
 * an area with more is read twice, its count, range and median still those of the values sorted, its mean and
 * standard deviation from compensated sums (a relative difference under 2e-6 from those of the values sorted: the
 * rounding of the last of the 7 digits of a float). The same for getRasterAreaStatsBoxJson.
 */
std::string getRasterAreaStatsJson(const std::string &filePath,
                                   int x0, int y0, int x1, int y1, const RasterValueOptions &options = {});

/**
 * The point (x, y) and the statistics of the rectangle (x0, y0)-(x1, y1) of the temperature grid of a thermal
 * image that GDAL does not read (an R-JPEG: its temperatures in degrees Celsius, and its raw counts when `raw`
 * is valid), with the options of the other rasters: a pixel whose raw sample (its raw count, else its
 * temperature) is the NoData value has no value, and a thermal formula gives the temperatures in its unit
 * (D-022). The schemas of getRasterPointJson and getRasterAreaStatsJson.
 */
std::string thermalGridPointJson(const TemperatureData &td, const RawThermalData &raw, int x, int y,
                                 const RasterValueOptions &options = {});
std::string thermalGridAreaStatsJson(const TemperatureData &td, const RawThermalData &raw, int x0, int y0, int x1,
                                     int y1, const RasterValueOptions &options = {});

/**
 * The same of the first band of a thermal raster GDAL reads (ThermalBandReader: its temperatures, and its raw counts
 * when it is UInt16), as the point and the area statistics read a thermal raster the band model cannot read: the point
 * reads its pixel alone, the area its pixels a window at a time, so that what they hold does not grow with the raster
 * (area_statistics_internal.h kMaxAreaBufferBytes). The JSON is that of thermalGridPointJson and
 * thermalGridAreaStatsJson on the grid of the band read whole.
 */
std::string thermalBandPointJson(const ThermalBandReader &band, int x, int y, const RasterValueOptions &options = {});
std::string thermalBandAreaStatsJson(const ThermalBandReader &band, int x0, int y0, int x1, int y1,
                                     const RasterValueOptions &options = {});

/**
 * Statistics over a WGS84 rectangle: the pixels whose centre is inside it (the same schema; "bounds" are the
 * pixels of the file that the rectangle covers).
 */
std::string getRasterAreaStatsBoxJson(const std::string &filePath, double minLon, double minLat, double maxLon,
                                      double maxLat, const RasterValueOptions &options = {});

/**
 * The metadata of a raster for a viewer (DDBGetRasterMetadata): statistics, the formulas it
 * can compute, the colormaps and the detected sensor.
 *
 * Output JSON schema (tests/contract/schemas/raster-metadata.schema.json):
 * {
 *   "statistics": {"<band>": {"min", "max", "mean", "std", "percentiles": {"p2", "p98"},
 *                              "histogram": {"counts", "edges", "bins", "below", "above"}  // formula only
 *                            }} | null,
 *   "algorithms": [{"id", "name", "expr", "help", "requiredBands", "category", "range"?, "disabled"?, "disabledReason"?}],
 *                  // requiredBands (new in 2.4): the names of the bands the formula reads (["Red", "NIR"]);
 *                  // category (new in 2.4): "rgb", "multispectral" or "thermal", the kind of raster it is for
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

/**
 * The rules of the values of the first band of a raster that the value endpoints (value info, point, area, profile)
 * share, so that they agree on what is a temperature (FR-RAS-14, D-022).
 *
 * bandValueConversion: the unit of the values and the conversion of a formula of the options into the unit it gives.
 * The unit of the band is its unit type; for a thermal raster that has none, degrees Celsius, except for values that are
 * no temperatures, which have no unit (""): the 8-bit colour preview of a picture, and the raw UInt16 counts of a sensor
 * that no calibration converts (calibrated: bandRawCalibration gave one). A UInt16 band with a scale or an offset (the
 * Landsat encoding) holds physical values, not raw counts. No formula keeps the unit of the band; a thermal formula
 * (CELSIUS, KELVIN, FAHRENHEIT) converts from it; any other formula, or a thermal formula on thermal values without a
 * unit (no temperatures), is a RefusedArgumentException ("No thermal calibration" for the latter, with the reason).
 *
 * bandRawCalibration: the Planck calibration that turns the raw UInt16 counts of a thermal band into temperatures, when
 * the file has one; none for any other band, nor for a band with a scale or an offset.
 */
ThermalFormulaConversion bandValueConversion(const DemContext &dem, bool isThermal, bool calibrated,
                                             const std::string &formula);
std::optional<ThermalCalibration> bandRawCalibration(const DemContext &dem, bool isThermal,
                                                     const std::string &filePath);

} // namespace ddb

#endif // RASTER_ANALYSIS_H
