/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "raster_analysis.h"
#include "cancellation.h"
#include "raster_region.h"
#include "raster_utils.h"
#include "json.h"
#include "thermal.h"
#include "sensorprofile.h"
#include "exceptions.h"
#include "artefact_cache.h"
#include "gdal_raii.h"
#include "dem.h"
#include "raster_stats.h"
#include "logger.h"
#include "vegetation.h"
#include "vis_render.h"

#include <gdal_priv.h>
#include <cpl_conv.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace ddb {

namespace {

// GDALDataType -> short string used in output JSON.
const char *dataTypeName(GDALDataType dt) {
    switch (dt) {
        case GDT_Byte: return "Byte";
        case GDT_UInt16: return "UInt16";
        case GDT_Int16: return "Int16";
        case GDT_UInt32: return "UInt32";
        case GDT_Int32: return "Int32";
        case GDT_Float32: return "Float32";
        case GDT_Float64: return "Float64";
        default: return "Unknown";
    }
}

// Best-effort detection of whether a raster should be treated as thermal.
// Uses the sensor profile database (EXIF make/model) and the direct-temperature
// heuristic (e.g. ODM thermal orthos with scale/offset metadata).
bool detectIsThermal(const std::string &filePath, std::string &outSensorId) {
    outSensorId.clear();
    try {
        auto &spm = SensorProfileManager::instance();
        auto det = spm.detectSensor(filePath);
        if (det.detected) {
            outSensorId = det.sensorId;
            if (det.sensorCategory == "thermal") return true;
        }
    } catch (const std::exception &e) {
        LOGD << "SensorProfileManager failed in detectIsThermal: " << e.what();
    }

    // Heuristic fallback (R-JPEG FLIR APP1 / DJI XMP / Landsat-style scale/offset).
    if (isThermalImage(filePath) || isDirectTemperatureRaster(filePath)) {
        return true;
    }

    return false;
}

// The rectangle of an area statistic, in pixels (inclusive)
struct AreaBounds {
    int x0, y0, x1, y1;

    // The part of the rectangle (corners in any order) inside a w x h raster, corners in order.
    // A rectangle with no pixel in the raster is a NoRasterDataException: it used to be clamped
    // onto the nearest edge, whose pixels it then measured.
    static AreaBounds within(int x0, int y0, int x1, int y1, int w, int h) {
        if (x0 > x1) std::swap(x0, x1);
        if (y0 > y1) std::swap(y0, y1);
        if (x1 < 0 || y1 < 0 || x0 >= w || y0 >= h)
            throw NoRasterDataException("No valid data in the area: (" + std::to_string(x0) + ", " +
                                        std::to_string(y0) + ") - (" + std::to_string(x1) + ", " +
                                        std::to_string(y1) + ") is outside the " + std::to_string(w) + "x" +
                                        std::to_string(h) + " raster");
        return {std::max(0, x0), std::max(0, y0), std::min(x1, w - 1), std::min(y1, h - 1)};
    }
};

// The values of an area with at least one valid pixel: a NoRasterDataException otherwise
std::vector<float> requireAreaValues(std::vector<float> values) {
    if (values.empty())
        throw NoRasterDataException("No valid data in the area: its pixels are all no data");
    return values;
}

// A NoRasterDataException when the pixel (x, y) is outside a w x h raster
void requirePixelInRaster(int x, int y, int w, int h) {
    if (x < 0 || x >= w || y < 0 || y >= h)
        throw NoRasterDataException("No valid data at the point: pixel (" + std::to_string(x) + ", " +
                                    std::to_string(y) + ") is outside the " + std::to_string(w) + "x" +
                                    std::to_string(h) + " raster");
}

// The calibration that turns the raw UInt16 samples of a thermal raster into temperatures, when the
// raster has one (the raw counts of a radiometric image: Planck); none for any other raster, whose values
// the band-value model (scale and offset) gives
std::optional<ThermalCalibration> rawCalibrationOf(const DemContext &dem, bool isThermal, const std::string &filePath) {
    if (!isThermal || GDALGetRasterDataType(dem.band) != GDT_UInt16) return std::nullopt;
    const ThermalCalibration cal = extractThermalCalibration(filePath);
    if (!cal.valid) return std::nullopt;
    return cal;
}

// The value of a raw sample of the band: a temperature for a calibrated thermal raster, else raw * scale +
// offset (FR-RAS-04)
double valueOfRaw(const DemContext &dem, const std::optional<ThermalCalibration> &cal, double raw) {
    return cal ? rawToTemperature(static_cast<uint16_t>(raw), *cal) : dem.toPhysical(raw);
}

// The unit the values of a point or an area are given in (D-022): the unit of the band without a formula; a
// thermal formula converts them from the unit of the band (thermal.h thermalFormulaConversion) to its own: CELSIUS
// to degrees Celsius, KELVIN to K, FAHRENHEIT to degrees Fahrenheit. Any other formula has no value at a point (a
// RefusedArgumentException).
struct ValueFormula {
    TemperatureConversion conversion; // the value in the unit of the formula
    std::string unit;                 // the unit of the values

    double apply(double v) const { return conversion.apply(v); }
};

ValueFormula valueFormulaOf(const std::string &bandUnit, const std::string &formula) {
    if (formula.empty()) return ValueFormula{{}, bandUnit};
    const ThermalFormulaConversion converted = thermalFormulaConversion(bandUnit, formula);
    return ValueFormula{converted.conversion, converted.unit};
}

// The unit of the values of a band: its unit type, degrees Celsius for a thermal raster that has none. The 8-bit
// band of a thermal picture is a colour preview, not temperatures: it has no unit (an R-JPEG whose raw thermal data
// cannot be read, FR-RAS-14); nor have the raw UInt16 counts of a thermal sensor that no calibration turns into
// temperatures (calibrated: rawCalibrationOf gave one)
std::string bandUnitOf(const DemContext &dem, bool isThermal, bool calibrated) {
    if (!dem.unit.empty() || !isThermal) return dem.unit;
    const GDALDataType type = GDALGetRasterDataType(dem.band);
    if (type == GDT_Byte || (type == GDT_UInt16 && !calibrated)) return "";
    return "\xC2\xB0\x43";
}

// The reason a thermal formula is refused on the values of a thermal picture that are no temperatures: the raw counts
// of a sensor without a calibration, or the colour preview of a picture without raw data (FR-RAS-14)
[[noreturn]] void refuseUncalibratedThermalFormula(const std::string &formula) {
    throw RefusedArgumentException(
        "No thermal calibration: the values of this thermal image are raw sensor counts without a unit, not "
        "temperatures, so the formula " + formula + " cannot give them (the raw counts of a DJI R-JPEG need DJI's own "
        "conversion, which is not available)");
}

// The formula of the values of a thermal image that has no unit (no temperatures): an unknown formula, or one that is
// not thermal, is refused as on any raster, and a thermal formula for want of a calibration; no formula gives the values
// as they are, without a unit
ValueFormula uncalibratedValueFormula(const std::string &formula) {
    const ValueFormula vf = valueFormulaOf("", formula);
    if (!formula.empty()) refuseUncalibratedThermalFormula(formula);
    return vf;
}

// The formula of the values of a band (point, area): uncalibratedValueFormula for a thermal band without a unit
ValueFormula bandValueFormula(const DemContext &dem, bool isThermal, bool calibrated, const std::string &formula) {
    const std::string unit = bandUnitOf(dem, isThermal, calibrated);
    if (isThermal && unit.empty()) return uncalibratedValueFormula(formula);
    return valueFormulaOf(unit, formula);
}

// The values of the pixels of the rectangle (inclusive bounds, inside the raster) that have one
std::vector<float> readAreaValues(const DemContext &dem, const std::optional<ThermalCalibration> &cal,
                                  const AreaBounds &b, const ValueFormula &vf) {
    const int roiW = b.x1 - b.x0 + 1;
    const int roiH = b.y1 - b.y0 + 1;
    std::vector<float> values = dem.readRaw(b.x0, b.y0, roiW, roiH, roiW, roiH);
    // A pixel with no value reads back as NaN
    values.erase(std::remove_if(values.begin(), values.end(), [&](float &v) {
        if (std::isnan(v)) return true;
        v = static_cast<float>(vf.apply(valueOfRaw(dem, cal, v)));
        // A raw count the calibration has no temperature for has no value either
        return std::isnan(v);
    }), values.end());
    return values;
}

// The statistics of the values (which must not be empty; sorted here), as the area endpoint
// answers them
std::string areaStatsJson(std::vector<float> values, const AreaBounds &b, const std::string &unit,
                          bool isThermal, GDALDataType type = GDT_Float32) {
    std::sort(values.begin(), values.end());
    const float vMin = values.front();
    const float vMax = values.back();

    double sum = 0;
    for (float v : values) sum += v;
    const double mean = sum / values.size();

    double sqSum = 0;
    for (float v : values) {
        double diff = v - mean;
        sqSum += diff * diff;
    }
    const double stddev = std::sqrt(sqSum / values.size());

    float median;
    const size_t n = values.size();
    if (n % 2 == 0) {
        median = (values[n / 2 - 1] + values[n / 2]) / 2.0f;
    } else {
        median = values[n / 2];
    }

    json result;
    result["min"] = tidyValue(vMin, type);
    result["max"] = tidyValue(vMax, type);
    result["mean"] = tidyValue(mean, type);
    result["stddev"] = tidyValue(stddev, type);
    result["median"] = tidyValue(median, type);
    result["pixelCount"] = static_cast<int>(values.size());
    result["bounds"] = {
        {"x0", b.x0}, {"y0", b.y0}, {"x1", b.x1}, {"y1", b.y1}
    };
    result["unit"] = unit;
    result["isThermal"] = isThermal;
    return result.dump();
}

// The thermal calibration of a raster as the value info reports it: the Planck constants, and only the measurement
// parameters the file says (no default is reported as if the camera had said it)
json calibrationJson(const ThermalCalibration &cal) {
    json result = {
        {"planckR1", cal.planckR1},
        {"planckB", cal.planckB},
        {"planckF", cal.planckF},
        {"planckO", cal.planckO},
        {"planckR2", cal.planckR2}
    };
    const auto put = [&result](const char *name, const std::optional<double> &value) {
        if (value) result[name] = *value;
    };
    put("emissivity", cal.emissivity);
    put("objectDistance", cal.objectDistance);
    put("reflectedApparentTemperature", cal.reflectedApparentTemperature);
    put("atmosphericTemperature", cal.atmosphericTemperature);
    put("relativeHumidity", cal.relativeHumidity);
    put("irWindowTemperature", cal.irWindowTemperature);
    put("irWindowTransmission", cal.irWindowTransmission);
    return result;
}

// The point result of the pixel (x, y) of the band: the value (a temperature for a calibrated thermal raster),
// the raw sample, the pixel. A pixel with no value (NoData, masked) is a NoRasterDataException.
json pointResult(const DemContext &dem, const std::optional<ThermalCalibration> &cal, int x, int y, bool isThermal,
                 const ValueFormula &vf) {
    const float raw = dem.readRaw(x, y, 1, 1, 1, 1)[0];
    if (std::isnan(raw))
        throw NoRasterDataException("No valid data at the point: the pixel (" + std::to_string(x) + ", " +
                                    std::to_string(y) + ") has no value");
    json result;
    const GDALDataType type = GDALGetRasterDataType(dem.band);
    const double value = valueOfRaw(dem, cal, raw);
    if (std::isnan(value))
        throw NoRasterDataException("No valid data at the point: the raw count of the pixel (" + std::to_string(x) +
                                    ", " + std::to_string(y) + ") has no temperature in the calibration");
    result["value"] = tidyValue(vf.apply(value), type);
    result["rawValue"] = tidyValue(raw, type);
    result["unit"] = vf.unit;
    result["x"] = x;
    result["y"] = y;
    result["isThermal"] = isThermal;
    return result;
}

// geoX and geoY of the pixel centre, and hasGeo: false without a geotransform or a projection
void setGeoPosition(json &result, bool hasGeoTransform, const double *geoTransform, const std::string &projection,
                    int x, int y) {
    if (!hasGeoTransform || projection.empty()) {
        result["hasGeo"] = false;
        return;
    }
    double geoX = 0, geoY = 0;
    pixelToGeo(x + 0.5, y + 0.5, geoTransform, projection, geoX, geoY);
    result["geoX"] = geoX;
    result["geoY"] = geoY;
    result["hasGeo"] = true;
}

// The raw sample of the pixel i of a thermal grid: its raw count when the image has them, else its temperature
float thermalGridRawSample(const TemperatureData &td, const RawThermalData &raw, size_t i) {
    if (raw.valid && raw.data.size() == td.temperatures.size()) return static_cast<float>(raw.data[i]);
    return td.temperatures[i];
}

// Whether the pixel i of a thermal grid has a value: a temperature a sensor can see, and a raw sample that is
// not the NoData value of the options (FR-RAS-31)
bool thermalGridHasValue(const TemperatureData &td, const RawThermalData &raw, size_t i,
                         const RasterValueOptions &options) {
    const float t = td.temperatures[i];
    if (!(std::isfinite(t) && t > -273.15f && t < 1000.0f)) return false;
    return !options.nodata || !isNodata(thermalGridRawSample(td, raw, i), BandNodata{true, *options.nodata});
}

// The point of a thermal image that GDAL does not read (an R-JPEG): its temperatures and raw data
std::string thermalImagePointJson(const std::string &filePath, int x, int y, const RasterValueOptions &options) {
    // A bad formula is refused before the image is read
    valueFormulaOf("\xC2\xB0\x43", options.formula);
    const TemperatureData td = readTemperatureData(filePath);
    if (td.temperatures.empty()) {
        throw AppException("Cannot read raster data from " + filePath);
    }
    return thermalGridPointJson(td, extractRawThermalData(filePath), x, y, options);
}

} // anonymous namespace

std::string thermalGridPointJson(const TemperatureData &td, const RawThermalData &raw, int x, int y,
                                 const RasterValueOptions &options) {
    // The temperatures of the grid are in degrees Celsius
    const ValueFormula vf = valueFormulaOf("\xC2\xB0\x43", options.formula);
    requirePixelInRaster(x, y, td.width, td.height);

    const size_t idx = static_cast<size_t>(y) * td.width + x;
    if (options.nodata && isNodata(thermalGridRawSample(td, raw, idx), BandNodata{true, *options.nodata}))
        throw NoRasterDataException("No valid data at the point: the pixel (" + std::to_string(x) + ", " +
                                    std::to_string(y) + ") has no value");
    const float value = td.temperatures[idx];

    json result;
    result["value"] = tidyValue(vf.apply(value), GDT_Float32);
    result["unit"] = vf.unit;
    result["x"] = x;
    result["y"] = y;
    result["isThermal"] = true;
    if (raw.valid && idx < raw.data.size()) {
        result["rawValue"] = raw.data[idx];
    } else {
        result["rawValue"] = value;
    }

    setGeoPosition(result, td.hasGeoTransform, td.geoTransform, td.projection, x, y);
    return result.dump();
}

std::string thermalGridAreaStatsJson(const TemperatureData &td, const RawThermalData &raw, int x0, int y0, int x1,
                                     int y1, const RasterValueOptions &options) {
    const ValueFormula vf = valueFormulaOf("\xC2\xB0\x43", options.formula);
    const AreaBounds bounds = AreaBounds::within(x0, y0, x1, y1, td.width, td.height);

    std::vector<float> values;
    values.reserve(static_cast<size_t>(bounds.x1 - bounds.x0 + 1) * (bounds.y1 - bounds.y0 + 1));
    for (int row = bounds.y0; row <= bounds.y1; row++) {
        for (int col = bounds.x0; col <= bounds.x1; col++) {
            const size_t i = static_cast<size_t>(row) * td.width + col;
            if (thermalGridHasValue(td, raw, i, options))
                values.push_back(static_cast<float>(vf.apply(td.temperatures[i])));
        }
    }

    return areaStatsJson(requireAreaValues(std::move(values)), bounds, vf.unit, true);
}

namespace {

// ------------------------------------------------------------------ the raw counts of an R-JPEG without temperatures

// The raw thermal grid of an R-JPEG whose raw counts no conversion turns into temperatures (a DJI camera, whose
// conversion is DJI's own and not available; a FLIR picture without the Planck constants in its tags). Its values are
// the raw counts, without a unit, as the raster API reports them (FR-RAS-14): not the colour preview, not degrees.

// Whether the pixel i of the raw grid has a value: its raw count is not the NoData value of the options (FR-RAS-31)
bool rawGridHasValue(const RawThermalData &raw, size_t i, const RasterValueOptions &options) {
    return !options.nodata || !isNodata(static_cast<float>(raw.data[i]), BandNodata{true, *options.nodata});
}

// The value info of the raw grid: the range of the raw counts, no unit, no calibration, not direct values. A thermal
// formula of the options gives no unit to raw counts: the info says what the image has (unit ""), whatever the formula
std::string rawGridValueInfoJson(const RawThermalData &raw, const std::string &sensorId,
                                 const RasterValueOptions &options) {
    uint16_t vMin = std::numeric_limits<uint16_t>::max();
    uint16_t vMax = 0;
    uint64_t validCount = 0;
    for (size_t i = 0; i < raw.data.size(); i++) {
        if (!rawGridHasValue(raw, i, options)) continue;
        vMin = std::min(vMin, raw.data[i]);
        vMax = std::max(vMax, raw.data[i]);
        validCount++;
    }
    if (validCount == 0)
        throw NoRasterDataException("Cannot compute statistics: no pixel of the image has a value");

    json result;
    result["width"] = raw.width;
    result["height"] = raw.height;
    result["bandCount"] = 1;
    result["dataType"] = dataTypeName(GDT_UInt16);
    result["valueMin"] = vMin;
    result["valueMax"] = vMax;
    result["unit"] = "";
    result["isThermal"] = true;
    result["isDirectValue"] = false;
    result["sensorId"] = sensorId;
    result["nodata"] = options.nodata ? json(*options.nodata) : json(nullptr);
    result["nodataOverridden"] = options.nodata.has_value();
    result["nodataInvalid"] = false;
    result["pixelCount"] = static_cast<uint64_t>(raw.width) * static_cast<uint64_t>(raw.height);
    result["validPixelCount"] = validCount;
    // The default stretch (D-041): the minimum to the maximum of the raw counts
    result["stretch"] = json::array({{{"min", vMin}, {"max", vMax > vMin ? vMax : vMin + 1}}});
    return result.dump();
}

// The point (x, y) of the raw grid: its raw count, without a unit. A thermal formula is refused (no calibration)
std::string rawGridPointJson(const RawThermalData &raw, int x, int y, const RasterValueOptions &options) {
    uncalibratedValueFormula(options.formula);
    requirePixelInRaster(x, y, raw.width, raw.height);

    const size_t idx = static_cast<size_t>(y) * raw.width + x;
    if (!rawGridHasValue(raw, idx, options))
        throw NoRasterDataException("No valid data at the point: the pixel (" + std::to_string(x) + ", " +
                                    std::to_string(y) + ") has no value");
    json result;
    result["value"] = raw.data[idx];
    result["rawValue"] = raw.data[idx];
    result["unit"] = "";
    result["x"] = x;
    result["y"] = y;
    result["isThermal"] = true;
    result["hasGeo"] = false;
    return result.dump();
}

// The statistics of the rectangle (x0, y0)-(x1, y1) of the raw grid: of its raw counts, without a unit. A thermal
// formula is refused (no calibration)
std::string rawGridAreaStatsJson(const RawThermalData &raw, int x0, int y0, int x1, int y1,
                                 const RasterValueOptions &options) {
    uncalibratedValueFormula(options.formula);
    const AreaBounds bounds = AreaBounds::within(x0, y0, x1, y1, raw.width, raw.height);

    std::vector<float> values;
    values.reserve(static_cast<size_t>(bounds.x1 - bounds.x0 + 1) * (bounds.y1 - bounds.y0 + 1));
    for (int row = bounds.y0; row <= bounds.y1; row++) {
        for (int col = bounds.x0; col <= bounds.x1; col++) {
            const size_t i = static_cast<size_t>(row) * raw.width + col;
            if (rawGridHasValue(raw, i, options)) values.push_back(static_cast<float>(raw.data[i]));
        }
    }
    return areaStatsJson(requireAreaValues(std::move(values)), bounds, "", true, GDT_UInt16);
}

} // anonymous namespace

namespace {

json formulaRasterStatistics(GDALDatasetH hDs, const std::string &formula, const BandFilter &filter);

// The default stretch of a band, as the info reports it: {"min", "max"} in the values of the band
json stretchEntry(double lo, double hi) {
    return {{"min", tidyValue(lo, GDT_Float32)}, {"max", tidyValue(hi, GDT_Float32)}};
}

// A band's statistics in its physical values (raw * scale + offset; the thermal calibration when the first
// band has one), as min, max, mean and standard deviation. The order of min and max follows the sign of the scale.
struct PhysicalStats {
    double min = 0, max = 0, mean = 0, stdDev = 0;
};

PhysicalStats physicalStats(const BandStatistics &s, GDALRasterBandH band, const std::optional<ThermalCalibration> &cal) {
    PhysicalStats p;
    if (cal) {
        p.min = rawToTemperature(static_cast<uint16_t>(s.min), *cal);
        p.max = rawToTemperature(static_cast<uint16_t>(s.max), *cal);
        if (p.min > p.max) std::swap(p.min, p.max);
        p.mean = rawToTemperature(static_cast<uint16_t>(std::lround(s.mean)), *cal);
        p.stdDev = s.stdDev;
        return p;
    }
    int hasScale = FALSE, hasOffset = FALSE;
    const double scale = GDALGetRasterScale(band, &hasScale);
    const double offset = GDALGetRasterOffset(band, &hasOffset);
    const double m = hasScale ? scale : 1.0;
    const double c = hasOffset ? offset : 0.0;
    p.min = s.min * m + c;
    p.max = s.max * m + c;
    if (p.min > p.max) std::swap(p.min, p.max);
    p.mean = s.mean * m + c;
    p.stdDev = s.stdDev * std::fabs(m);
    return p;
}

// The default stretch of the raster (D-041), one {"min", "max"} per band, or one for an index: the
// 2nd to the 98th percentile of the values of the formula, clipped to its range. Empty for a Byte raster
// of several bands (an RGB image has no stretch). A single band (a DEM, a thermal raster) is stretched from
// its minimum to its maximum; the bands of a raster that is not Byte each over the mean +- 2.33 standard
// deviations, clamped to the minimum and the maximum (stretchRange).
json defaultStretchJson(const std::string &filePath, GDALDatasetH hDs, const DemContext &dem,
                        const std::optional<ThermalCalibration> &cal, const RasterValueOptions &options,
                        const ValueFormula &unit) {
    auto &ve = VegetationEngine::instance();
    // A thermal formula (D-022): the stretch of the first band, in the unit of the formula
    if (isThermalFormulaId(options.formula)) {
        const std::optional<double> nodataOverride =
            dem.nodataOverridden ? std::optional<double>(dem.nodata.value) : std::nullopt;
        const PhysicalStats p = physicalStats(RasterStatsProvider::instance().get(filePath, hDs, 1, nodataOverride),
                                              dem.band, cal);
        const double lo = unit.apply(p.min);
        double hi = unit.apply(p.max);
        if (!(hi > lo)) hi = lo + 1.0;
        return json::array({stretchEntry(lo, hi)});
    }
    if (!options.formula.empty()) {
        const VegetationFormula *formula = ve.getFormula(options.formula);
        // A thermal formula keeps the stretch of the band; an index has its own
        if (formula != nullptr && formula->requiredBands != "T") {
            const BandFilter filter = ve.autoDetectFilter(filePath);
            const json stats = formulaRasterStatistics(hDs, options.formula, filter);
            if (stats.is_null() || !stats.contains("1"))
                return json::array();
            const json &s = stats["1"];
            double lo = s["percentiles"]["p2"].get<double>();
            double hi = s["percentiles"]["p98"].get<double>();
            if (formula->hasRange) {
                lo = std::max(lo, formula->rangeMin);
                hi = std::min(hi, formula->rangeMax);
            }
            if (!(hi > lo)) hi = lo + 1.0;
            return json::array({stretchEntry(lo, hi)});
        }
    }

    const int dataBands = dataBandCount(hDs);
    const bool isByte = GDALGetRasterDataType(dem.band) == GDT_Byte;
    auto &provider = RasterStatsProvider::instance();
    const std::optional<double> nodata = dem.nodataOverridden ? std::optional<double>(dem.nodata.value) : std::nullopt;
    json stretch = json::array();
    if (dataBands <= 1) {
        const PhysicalStats p = physicalStats(provider.get(filePath, hDs, 1, nodata), dem.band, cal);
        double hi = p.max;
        if (!(hi > p.min)) hi = p.min + 1.0;
        stretch.push_back(stretchEntry(p.min, hi));
    } else if (!isByte) {
        for (int b = 1; b <= dataBands; b++) {
            const GDALRasterBandH band = GDALGetRasterBand(hDs, b);
            const BandStatistics raw = provider.get(filePath, hDs, b, nodata);
            const PhysicalStats p = physicalStats(raw, band, b == 1 ? cal : std::nullopt);
            BandStatistics physical;
            physical.min = p.min;
            physical.max = p.max;
            physical.mean = p.mean;
            physical.stdDev = p.stdDev;
            const auto [lo, hi] = stretchRange(physical);
            stretch.push_back(stretchEntry(lo, hi));
        }
    }
    return stretch;
}

} // anonymous namespace

namespace {

// The value info of a grid of temperatures (degrees Celsius): the range, the calibration and the stretch, in the unit
// of the thermal formula of the options when it has one (D-022). raw are the raw counts of the camera, when the
// caller needs them for the NoData value of the options (FR-RAS-31).
std::string thermalGridValueInfoJson(const TemperatureData &td, const RawThermalData &raw, const ThermalCalibration &cal,
                                     bool isDirect, const std::string &sensorId, const RasterValueOptions &options) {
    // The temperatures of the grid are in degrees Celsius; a thermal formula gives them in its unit (D-022)
    const ValueFormula unit = valueFormulaOf("\xC2\xB0\x43", isThermalFormulaId(options.formula) ? options.formula : "");
    float vMin = std::numeric_limits<float>::max();
    float vMax = std::numeric_limits<float>::lowest();
    uint64_t validCount = 0;
    for (size_t i = 0; i < td.temperatures.size(); i++) {
        if (!thermalGridHasValue(td, raw, i, options)) continue;
        const float t = td.temperatures[i];
        vMin = std::min(vMin, t);
        vMax = std::max(vMax, t);
        validCount++;
    }
    if (validCount == 0)
        throw NoRasterDataException("Cannot compute statistics: no pixel of the image has a value");

    json result;
    result["width"] = td.width;
    result["height"] = td.height;
    result["bandCount"] = 1;
    result["dataType"] = dataTypeName(GDT_Unknown);
    result["valueMin"] = unit.apply(vMin);
    result["valueMax"] = unit.apply(vMax);
    result["unit"] = unit.unit;
    result["isThermal"] = true;
    result["isDirectValue"] = isDirect;
    result["sensorId"] = sensorId;
    if (cal.valid) result["calibration"] = calibrationJson(cal);
    result["nodata"] = options.nodata ? json(*options.nodata) : json(nullptr);
    result["nodataOverridden"] = options.nodata.has_value();
    result["nodataInvalid"] = false;
    result["pixelCount"] = static_cast<uint64_t>(td.width) * static_cast<uint64_t>(td.height);
    result["validPixelCount"] = validCount;
    result["stretch"] = json::array({stretchEntry(unit.apply(vMin), unit.apply(vMax > vMin ? vMax : vMin + 1.0))});
    return result.dump();
}

} // anonymous namespace

std::string getRasterValueInfoJson(const std::string &filePath, const RasterValueOptions &options) {
    cancel::throwIfCanceled();
    std::string sensorId;
    const bool isThermal = detectIsThermal(filePath, sensorId);
    ThermalCalibration cal;
    if (isThermal) cal = extractThermalCalibration(filePath);

    const bool isDirect = isThermal ? isDirectTemperatureRaster(filePath) : true;

    // An R-JPEG: the temperatures come from its raw thermal data and its calibration, not from the colour bands
    // GDAL reads (the preview, 0-255, FR-RAS-14); without a conversion of its raw counts (DJI), the raw counts
    // without a unit
    if (isThermal) {
        if (const auto rjpeg = readRJpegThermal(filePath)) {
            if (!rjpeg->hasTemperatures()) return rawGridValueInfoJson(rjpeg->raw, sensorId, options);
            return thermalGridValueInfoJson(rjpeg->grid, rjpeg->raw, rjpeg->calibration, false, sensorId, options);
        }
    }

    json result;
    bool hasStats = false;
    try {
        // The values of the raster come from this file (the source file the Registry names), as the band
        // model reads them: the NoData value of the caller, the scale and the offset (FR-XC-24)
        const DemContext dem = openDem(filePath, 1, BandValueOptions{options.nodata}, false);
        const GDALDatasetH hDs = dem.handle();
        const GDALDataType dt = GDALGetRasterDataType(dem.band);
        const std::optional<ThermalCalibration> rawCal =
            (isThermal && dt == GDT_UInt16 && cal.valid) ? std::optional<ThermalCalibration>(cal) : std::nullopt;

        // The exact statistics of the pixels that have a value: the range of the whole raster
        // (FR-RAS-07), through the provider
        const BandStatistics raw = RasterStatsProvider::instance().get(filePath, hDs, 1, options.nodata);
        const PhysicalStats p = physicalStats(raw, dem.band, rawCal);

        // The unit of the values: the band's, else degrees Celsius for a thermal raster that has none; a thermal
        // formula gives the range in its own unit (D-022). Values that are no temperatures (raw counts without a
        // calibration, a colour preview) have no unit, whatever the formula
        const std::string bandUnit = bandUnitOf(dem, isThermal, rawCal.has_value());
        const ValueFormula unit = isThermalFormulaId(options.formula) && !(isThermal && bandUnit.empty())
                                      ? valueFormulaOf(bandUnit, options.formula)
                                      : ValueFormula{{}, bandUnit};

        result["width"] = dem.width;
        result["height"] = dem.height;
        result["bandCount"] = GDALGetRasterCount(hDs);
        result["dataType"] = dataTypeName(dt);
        result["valueMin"] = tidyValue(unit.apply(p.min), dt);
        result["valueMax"] = tidyValue(unit.apply(p.max), dt);
        result["unit"] = unit.unit;
        result["isThermal"] = isThermal;
        result["isDirectValue"] = isDirect;
        result["sensorId"] = sensorId;
        // The colour preview of a picture is not what a calibration converts
        if (isThermal && cal.valid && dt != GDT_Byte)
            result["calibration"] = calibrationJson(cal);

        // The NoData value that applies, and whether the band can hold it (65536 in a UInt16 band cannot:
        // no pixel is NoData then, and the Hub offers the value field)
        result["nodata"] = dem.nodata.has && std::isfinite(dem.nodata.value) ? json(dem.nodata.value) : json(nullptr);
        result["nodataOverridden"] = dem.nodataOverridden;
        result["nodataInvalid"] = dem.nodataInvalid;
        if (dem.hasScale) {
            result["scale"] = dem.scale;
            result["offset"] = dem.offset;
        }

        // How many pixels have a value, for the size of an export (D-023): of the whole raster and, for a
        // window (bbox, WGS84), the source pixels inside it
        result["pixelCount"] = static_cast<uint64_t>(dem.width) * static_cast<uint64_t>(dem.height);
        result["validPixelCount"] = raw.validCount;
        if (options.bbox) {
            if (!dem.hasGeoTransform) throw InvalidArgsException("The raster has no geotransform: no window can be placed on it");
            const auto &b = *options.bbox;
            int x0, y0, x1, y1;
            dem.wgs84BoxToPixelBounds(b[0], b[1], b[2], b[3], x0, y0, x1, y1);
            const int64_t cols = std::max<int64_t>(0, std::min<int64_t>(x1, dem.width - 1) - std::max<int64_t>(x0, 0) + 1);
            const int64_t rows = std::max<int64_t>(0, std::min<int64_t>(y1, dem.height - 1) - std::max<int64_t>(y0, 0) + 1);
            result["windowPixelCount"] = static_cast<uint64_t>(cols * rows);
        }

        result["stretch"] = defaultStretchJson(filePath, hDs, dem, rawCal, options, unit);
        hasStats = true;
    } catch (const GDALException &e) {
        // A file GDAL cannot read, or no pixel with a value: the R-JPEG fallback below, else the failure
        LOGD << "getRasterValueInfoJson: " << e.what();
        if (!isThermal) throw;
        result = json::object();
    }

    // Fallback for small R-JPEG thermal images not fully handled by GDAL stats.
    if (!hasStats && isThermal) {
        TemperatureData td = readTemperatureData(filePath);
        if (!td.temperatures.empty()) {
            // A raw sample equal to the NoData value of the options has no value (FR-RAS-31)
            const RawThermalData raw = options.nodata ? extractRawThermalData(filePath) : RawThermalData();
            return thermalGridValueInfoJson(td, raw, cal, isDirect, sensorId, options);
        }
    }

    if (!hasStats) {
        throw AppException("Cannot read raster data from " + filePath);
    }
    return result.dump();
}

namespace {

// The point at pixel (x, y) of a raster GDAL reads; throws GDALException when GDAL cannot open it
std::string pointAtPixel(const std::string &filePath, int x, int y, bool isThermal, const RasterValueOptions &options) {
    // A raster without a geotransform has its pixels and no map: the pixel coordinates still work
    const DemContext dem = openDem(filePath, 1, BandValueOptions{options.nodata}, false);
    const auto cal = rawCalibrationOf(dem, isThermal, filePath);
    const ValueFormula vf = bandValueFormula(dem, isThermal, cal.has_value(), options.formula);
    requirePixelInRaster(x, y, dem.width, dem.height);
    json result = pointResult(dem, cal, x, y, isThermal, vf);
    double geoTransform[6];
    const char *proj = GDALGetProjectionRef(dem.handle());
    setGeoPosition(result, GDALGetGeoTransform(dem.handle(), geoTransform) == CE_None, geoTransform,
                   proj != nullptr ? proj : "", x, y);
    return result.dump();
}

} // anonymous namespace

std::string getRasterPointJson(const std::string &filePath, int x, int y, const RasterValueOptions &options) {
    cancel::throwIfCanceled();
    std::string sensorId;
    const bool isThermal = detectIsThermal(filePath, sensorId);

    // An R-JPEG: the temperature of the raw thermal grid, not the colour byte of the preview (FR-RAS-14), or its raw
    // count without a unit when no conversion is known (DJI). The pixel is a pixel of that grid (the picture has no
    // geotransform)
    if (isThermal) {
        valueFormulaOf("\xC2\xB0\x43", options.formula); // a bad formula is refused before the image is read
        if (const auto rjpeg = readRJpegThermal(filePath)) {
            if (!rjpeg->hasTemperatures()) return rawGridPointJson(rjpeg->raw, x, y, options);
            return thermalGridPointJson(rjpeg->grid, rjpeg->raw, x, y, options);
        }
    }

    try {
        return pointAtPixel(filePath, x, y, isThermal, options);
    } catch (const GDALException &e) {
        LOGD << "getRasterPointJson: " << e.what();
    }

    // Fallback: R-JPEG (small thermal images).
    if (!isThermal) {
        throw AppException("Cannot read raster data from " + filePath);
    }
    return thermalImagePointJson(filePath, x, y, options);
}

std::string getRasterPointAtJson(const std::string &filePath, double lon, double lat, const RasterValueOptions &options) {
    cancel::throwIfCanceled();
    std::string sensorId;
    const bool isThermal = detectIsThermal(filePath, sensorId);

    // The map coordinates are converted to the pixel of the file by the one helper of the raster
    // (DemContext::wgs84ToPixelIndex), the pixel read is the pixel under the point
    const DemContext dem = openDem(filePath, 1, BandValueOptions{options.nodata});
    const auto cal = rawCalibrationOf(dem, isThermal, filePath);
    const ValueFormula vf = bandValueFormula(dem, isThermal, cal.has_value(), options.formula);
    int x = 0, y = 0;
    dem.wgs84ToPixelIndex(lon, lat, x, y);
    requirePixelInRaster(x, y, dem.width, dem.height);

    json result = pointResult(dem, cal, x, y, isThermal, vf);
    double geoTransform[6];
    const char *proj = GDALGetProjectionRef(dem.handle());
    setGeoPosition(result, GDALGetGeoTransform(dem.handle(), geoTransform) == CE_None, geoTransform,
                   proj != nullptr ? proj : "", x, y);
    // The point that was asked for
    result["lon"] = lon;
    result["lat"] = lat;
    return result.dump();
}

namespace {

std::string areaOfBounds(const DemContext &dem, const std::optional<ThermalCalibration> &cal, bool isThermal,
                         const AreaBounds &bounds, const ValueFormula &vf) {
    return areaStatsJson(requireAreaValues(readAreaValues(dem, cal, bounds, vf)), bounds, vf.unit, isThermal,
                         GDALGetRasterDataType(dem.band));
}

} // anonymous namespace

std::string getRasterAreaStatsJson(const std::string &filePath,
                                   int x0, int y0, int x1, int y1, const RasterValueOptions &options) {
    cancel::throwIfCanceled();
    std::string sensorId;
    const bool isThermal = detectIsThermal(filePath, sensorId);

    // An R-JPEG: the temperatures of the raw thermal grid (FR-RAS-14), or its raw counts without a unit when no
    // conversion is known (DJI)
    if (isThermal) {
        valueFormulaOf("\xC2\xB0\x43", options.formula); // a bad formula is refused before the image is read
        if (const auto rjpeg = readRJpegThermal(filePath)) {
            if (!rjpeg->hasTemperatures()) return rawGridAreaStatsJson(rjpeg->raw, x0, y0, x1, y1, options);
            return thermalGridAreaStatsJson(rjpeg->grid, rjpeg->raw, x0, y0, x1, y1, options);
        }
    }

    try {
        const DemContext dem = openDem(filePath, 1, BandValueOptions{options.nodata}, false);
        const auto cal = rawCalibrationOf(dem, isThermal, filePath);
        const ValueFormula vf = bandValueFormula(dem, isThermal, cal.has_value(), options.formula);
        const AreaBounds bounds = AreaBounds::within(x0, y0, x1, y1, dem.width, dem.height);
        return areaOfBounds(dem, cal, isThermal, bounds, vf);
    } catch (const GDALException &e) {
        LOGD << "getRasterAreaStatsJson: " << e.what();
    }

    // Fallback: R-JPEG thermal only.
    if (!isThermal) {
        throw AppException("Cannot read raster data from " + filePath);
    }

    // A bad formula is refused before the image is read
    valueFormulaOf("\xC2\xB0\x43", options.formula);
    const TemperatureData td = readTemperatureData(filePath);
    if (td.temperatures.empty()) {
        throw AppException("Cannot read raster data from " + filePath);
    }
    const RawThermalData raw = options.nodata ? extractRawThermalData(filePath) : RawThermalData();
    return thermalGridAreaStatsJson(td, raw, x0, y0, x1, y1, options);
}

std::string getRasterAreaStatsBoxJson(const std::string &filePath, double minLon, double minLat, double maxLon,
                                      double maxLat, const RasterValueOptions &options) {
    cancel::throwIfCanceled();
    std::string sensorId;
    const bool isThermal = detectIsThermal(filePath, sensorId);

    const DemContext dem = openDem(filePath, 1, BandValueOptions{options.nodata});
    const auto cal = rawCalibrationOf(dem, isThermal, filePath);
    const ValueFormula vf = bandValueFormula(dem, isThermal, cal.has_value(), options.formula);
    int x0, y0, x1, y1;
    dem.wgs84BoxToPixelBounds(minLon, minLat, maxLon, maxLat, x0, y0, x1, y1);
    // The pixels of the rectangle inside the raster; none is a NoRasterDataException
    const AreaBounds bounds = AreaBounds::within(x0, y0, x1, y1, dem.width, dem.height);
    return areaOfBounds(dem, cal, isThermal, bounds, vf);
}

// ------------------------------------------------------------------ raster metadata

namespace {

// The statistics of the values of a formula (the NODATA_SENTINEL ones are no data), as the
// statistics of band "1": min, max, mean, std, p2 and p98, and a histogram of 255 bins over
// formulaHistogramRange. Null when no pixel has a value.
json formulaStatistics(const std::vector<float> &formulaResult, const VegetationFormula *formula) {
    std::vector<double> valid;
    valid.reserve(formulaResult.size());
    double sum = 0, sumSq = 0;
    for (const float f : formulaResult) {
        if (f != NODATA_SENTINEL) {
            double v = static_cast<double>(f);
            valid.push_back(v);
            sum += v;
            sumSq += v * v;
        }
    }
    if (valid.empty())
        return json();

    const double count = static_cast<double>(valid.size());
    double mean = sum / count;
    double variance = (sumSq / count) - (mean * mean);
    double stddev = std::sqrt(std::max(0.0, variance));

    std::sort(valid.begin(), valid.end());
    const double fMin = valid.front();
    const double fMax = valid.back();
    double p2 = valid[static_cast<size_t>(count * 0.02)];
    double p98 = valid[std::min(valid.size() - 1, static_cast<size_t>(count * 0.98))];

    std::optional<std::pair<double, double>> validRange;
    if (formula != nullptr && formula->hasRange)
        validRange = std::make_pair(formula->rangeMin, formula->rangeMax);
    const auto [lo, hi] = formulaHistogramRange(fMin, fMax, p2, p98, validRange);
    constexpr int bins = 255;
    const ValueHistogram histogram = binValues(valid, lo, hi, bins);

    json stats;
    stats["1"] = {
        {"min", fMin}, {"max", fMax}, {"mean", mean}, {"std", stddev},
        {"percentiles", {{"p2", p2}, {"p98", p98}}},
        {"histogram", {{"counts", histogram.counts}, {"edges", histogram.edges}, {"bins", bins},
                       {"below", histogram.below}, {"above", histogram.above}}}
    };
    return stats;
}

// The statistics of a formula over the raster sampled at 512 x 512 at most
json formulaRasterStatistics(GDALDatasetH hDs, const std::string &formula, const BandFilter &filter) {
    const int width = GDALGetRasterXSize(hDs);
    const int height = GDALGetRasterYSize(hDs);

    // Sample at reduced resolution for performance
    const int sampleW = std::min(width, 512);
    const int sampleH = std::min(height, 512);

    const FormulaPlan plan = planFormula(hDs, formula, filter, "", false);
    FormulaBuffers buffers;
    computeFormulaValues(hDs, plan, RasterWindow{0, 0, width, height, sampleW, sampleH}, buffers);
    return formulaStatistics(buffers.result, plan.formula);
}

// The statistics of every band the provider can compute them for (exact: FR-RAS-07), by band number; null
// when none
json bandStatistics(const std::string &path, GDALDatasetH hDs) {
    json stats;
    const int nBands = GDALGetRasterCount(hDs);
    for (int i = 1; i <= nBands; i++) {
        BandStatistics s;
        try {
            s = RasterStatsProvider::instance().get(path, hDs, i);
        } catch (const GDALException &) {
            // A band with no pixel that has a value has no statistics
            cancel::throwIfCanceled();
            continue;
        }
        stats[std::to_string(i)] = {
            {"min", s.min}, {"max", s.max}, {"mean", s.mean}, {"std", s.stdDev},
            {"percentiles", {{"p2", s.mean - 2.33 * s.stdDev}, {"p98", s.mean + 2.33 * s.stdDev}}}
        };
    }
    return stats;
}

// Every formula of the engine, those the raster cannot compute (with this band filter and this
// data) marked disabled with their reason
json algorithmList(const VegetationEngine &ve, GDALDatasetH hDs, const BandFilter &filter) {
    const RasterDataKind dataKind = rasterDataKind(hDs);
    json algorithms = json::array();
    for (const auto &f : ve.getAllFormulas()) {
        json fj = {
            {"id", f.id}, {"name", f.name}, {"expr", f.expr},
            {"help", f.help}
        };
        if (f.hasRange) {
            fj["range"] = {f.rangeMin, f.rangeMax};
        }
        const std::string reason = VegetationEngine::unavailableReason(f, filter, dataKind);
        if (!reason.empty()) {
            fj["disabled"] = true;
            fj["disabledReason"] = reason;
        }
        algorithms.push_back(fj);
    }
    return algorithms;
}

} // anonymous namespace

std::pair<double, double> formulaHistogramRange(double min, double max, double p2, double p98,
                                                const std::optional<std::pair<double, double>> &validRange) {
    double lo = min;
    double hi = max;
    if (validRange) {
        lo = std::max(lo, validRange->first);
        hi = std::min(hi, validRange->second);
    }
    const double spread = p98 - p2;
    if (spread > 0) {
        lo = std::max(lo, p2 - spread);
        hi = std::min(hi, p98 + spread);
    }
    if (!(hi > lo)) {
        lo = min;
        hi = max;
    }
    if (!(hi > lo))
        hi = lo + 1.0;
    return {lo, hi};
}

ValueHistogram binValues(const std::vector<double> &values, double lo, double hi, int bins) {
    ValueHistogram h;
    h.counts.assign(static_cast<size_t>(bins), 0);
    h.edges.resize(static_cast<size_t>(bins) + 1);
    const double width = (hi - lo) / bins;
    for (int i = 0; i < bins; i++)
        h.edges[i] = lo + width * i;
    h.edges[bins] = hi;
    for (const double v : values) {
        if (v < lo) {
            h.below++;
        } else if (v > hi) {
            h.above++;
        } else {
            // The bin of v is the one whose edges hold it: the same width as the edges
            const int idx = std::min(bins - 1, static_cast<int>(std::floor((v - lo) / width)));
            h.counts[idx]++;
        }
    }
    return h;
}

std::string getRasterMetadataJson(const std::string &path, const std::string &formula, const std::string &bandFilter) {
    cancel::throwIfCanceled();
    // Owned: the formula, statistics and sensor steps below can throw while it is open
    auto dataset = raster::openInputRaster(path);
    if (!dataset)
        throw GDALException("Cannot open " + path);
    GDALDatasetH hDs = datasetHandle(dataset);

    auto &ve = VegetationEngine::instance();

    // Determine effective band filter
    const BandFilter effectiveFilter = !bandFilter.empty()
                                           ? VegetationEngine::parseFilter(bandFilter, GDALGetRasterCount(hDs))
                                           : ve.autoDetectFilter(path);

    json result;
    result["statistics"] = !formula.empty() ? formulaRasterStatistics(hDs, formula, effectiveFilter)
                                            : bandStatistics(path, hDs);

    // Available algorithms for this band filter and this data
    // Include all formulas, mark the ones the raster cannot compute as disabled
    result["algorithms"] = algorithmList(ve, hDs, effectiveFilter);
    result["colormaps"] = ve.getColormapsJson();
    result["autoBands"] = {{"filter", effectiveFilter.id}, {"match", !effectiveFilter.id.empty()}};
    result["detectedSensor"] = SensorProfileManager::instance().detectSensor(path).sensorId;

    dataset.reset();
    return result.dump();
}

std::string getRasterInfoJson(const std::string &path) {
    json info = json::parse(SensorProfileManager::instance().getRasterInfoJson(path));
    // The indices the WMS vegetation-index layer renders from this raster
    info["indexLayer"] = rasterIndexLayerIndices(path);
    return info.dump();
}


} // namespace ddb
