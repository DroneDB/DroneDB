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
#include "logger.h"
#include "vegetation.h"
#include "vis_render.h"

#include <gdal_priv.h>
#include <cpl_conv.h>

#include <algorithm>
#include <cmath>
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

    // The rectangle clamped to a w x h raster, corners in order
    static AreaBounds clamped(int x0, int y0, int x1, int y1, int w, int h) {
        x0 = std::max(0, std::min(x0, w - 1));
        y0 = std::max(0, std::min(y0, h - 1));
        x1 = std::max(0, std::min(x1, w - 1));
        y1 = std::max(0, std::min(y1, h - 1));
        if (x0 > x1) std::swap(x0, x1);
        if (y0 > y1) std::swap(y0, y1);
        return {x0, y0, x1, y1};
    }
};

// Reads the first band over the rectangle as floats (raw thermal values converted to
// temperatures when the calibration allows); nothing when GDAL cannot read it
std::optional<std::vector<float>> readAreaBand(GDALRasterBandH hBand, GDALDataType dt, const AreaBounds &b,
                                               bool needsConversion, const ThermalCalibration &cal) {
    const int roiW = b.x1 - b.x0 + 1;
    const int roiH = b.y1 - b.y0 + 1;
    const size_t roiCount = static_cast<size_t>(roiW) * roiH;
    std::vector<float> roiData(roiCount);

    // The read stops when the call is canceled
    cancel::GdalProgress progress;
    GDALRasterIOExtraArg extra = progress.rasterIOExtraArg();
    const auto failed = [] {
        cancel::throwIfCanceled();
        return std::optional<std::vector<float>>();
    };

    if (dt == GDT_UInt16) {
        std::vector<uint16_t> rawRoi(roiCount);
        if (GDALRasterIOEx(hBand, GF_Read, b.x0, b.y0, roiW, roiH,
                           rawRoi.data(), roiW, roiH,
                           GDT_UInt16, 0, 0, &extra) != CE_None)
            return failed();
        for (size_t i = 0; i < roiCount; i++) {
            roiData[i] = needsConversion
                ? static_cast<float>(rawToTemperature(rawRoi[i], cal))
                : static_cast<float>(rawRoi[i]);
        }
        return roiData;
    }

    // Floating point and other integer types: read as Float32.
    if (GDALRasterIOEx(hBand, GF_Read, b.x0, b.y0, roiW, roiH,
                       roiData.data(), roiW, roiH, GDT_Float32, 0, 0, &extra) != CE_None)
        return failed();
    return roiData;
}

// The statistics of the values (which must not be empty; sorted here), as the area endpoint
// answers them
std::string areaStatsJson(std::vector<float> values, const AreaBounds &b, const std::string &unit,
                          bool isThermal) {
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
    result["min"] = vMin;
    result["max"] = vMax;
    result["mean"] = mean;
    result["stddev"] = stddev;
    result["median"] = median;
    result["pixelCount"] = static_cast<int>(values.size());
    result["bounds"] = {
        {"x0", b.x0}, {"y0", b.y0}, {"x1", b.x1}, {"y1", b.y1}
    };
    result["unit"] = unit;
    result["isThermal"] = isThermal;
    return result.dump();
}

// The thermal calibration of a raster as the value info reports it
json calibrationJson(const ThermalCalibration &cal) {
    return {
        {"planckR1", cal.planckR1},
        {"planckB", cal.planckB},
        {"planckF", cal.planckF},
        {"planckO", cal.planckO},
        {"planckR2", cal.planckR2},
        {"emissivity", cal.emissivity},
        {"objectDistance", cal.objectDistance},
        {"reflectedApparentTemperature", cal.reflectedApparentTemperature},
        {"atmosphericTemperature", cal.atmosphericTemperature},
        {"relativeHumidity", cal.relativeHumidity},
        {"irWindowTemperature", cal.irWindowTemperature},
        {"irWindowTransmission", cal.irWindowTransmission}
    };
}

// What a pixel of the first band holds: the value (a temperature for a calibrated thermal raster)
// and, for a 16-bit raster, the raw sample
struct PixelValue {
    float value = 0;
    float rawValue = 0;
    bool hasRaw = false;
};

// The pixel (x, y) of the band, or nothing when GDAL cannot read it
std::optional<PixelValue> readPixel(GDALRasterBandH hBand, int x, int y, bool isThermal,
                                    const std::string &filePath) {
    const GDALDataType dt = GDALGetRasterDataType(hBand);
    PixelValue pixel;

    if (dt == GDT_UInt16) {
        uint16_t raw;
        if (GDALRasterIO(hBand, GF_Read, x, y, 1, 1, &raw, 1, 1, GDT_UInt16, 0, 0) != CE_None)
            return std::nullopt;
        pixel.rawValue = static_cast<float>(raw);
        pixel.hasRaw = true;
        pixel.value = static_cast<float>(raw);
        if (isThermal) {
            const ThermalCalibration cal = extractThermalCalibration(filePath);
            if (cal.valid)
                pixel.value = static_cast<float>(rawToTemperature(raw, cal));
        }
        return pixel;
    }

    // Floating point and other integer types: read as Float32 passthrough.
    float pixVal;
    if (GDALRasterIO(hBand, GF_Read, x, y, 1, 1, &pixVal, 1, 1, GDT_Float32, 0, 0) != CE_None)
        return std::nullopt;
    pixel.value = pixVal;
    pixel.rawValue = pixVal;
    return pixel;
}

json pointResult(const PixelValue &pixel, int x, int y, bool isThermal) {
    json result;
    result["value"] = pixel.value;
    result["rawValue"] = pixel.hasRaw ? pixel.rawValue : pixel.value;
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

// The point of a thermal image that GDAL does not read (an R-JPEG): its temperatures and raw data
std::string thermalImagePointJson(const std::string &filePath, int x, int y) {
    TemperatureData td = readTemperatureData(filePath);
    if (td.temperatures.empty()) {
        throw AppException("Cannot read raster data from " + filePath);
    }

    if (x < 0 || x >= td.width || y < 0 || y >= td.height) {
        throw InvalidArgsException("Pixel coordinates out of bounds: (" +
            std::to_string(x) + ", " + std::to_string(y) + ") for image " +
            std::to_string(td.width) + "x" + std::to_string(td.height));
    }

    size_t idx = static_cast<size_t>(y) * td.width + x;
    float value = td.temperatures[idx];

    json result;
    result["value"] = value;
    result["x"] = x;
    result["y"] = y;
    result["isThermal"] = true;

    RawThermalData raw = extractRawThermalData(filePath);
    if (raw.valid && idx < raw.data.size()) {
        result["rawValue"] = raw.data[idx];
    } else {
        result["rawValue"] = value;
    }

    setGeoPosition(result, td.hasGeoTransform, td.geoTransform, td.projection, x, y);
    return result.dump();
}

} // anonymous namespace

std::string getRasterValueInfoJson(const std::string &filePath) {
    cancel::throwIfCanceled();
    std::string sensorId;
    const bool isThermal = detectIsThermal(filePath, sensorId);
    ThermalCalibration cal;
    if (isThermal) cal = extractThermalCalibration(filePath);

    const bool isDirect = isThermal ? isDirectTemperatureRaster(filePath) : true;

    int width = 0, height = 0, bandCount = 0;
    GDALDataType dt = GDT_Unknown;
    std::string unit;
    float vMin = 0.f, vMax = 0.f;
    bool hasStats = false;

    if (const auto dataset = raster::openInputRaster(filePath)) {
        GDALDatasetH hDs = datasetHandle(dataset);
        width = GDALGetRasterXSize(hDs);
        height = GDALGetRasterYSize(hDs);
        bandCount = GDALGetRasterCount(hDs);

        if (bandCount >= 1) {
            GDALRasterBandH hBand = GDALGetRasterBand(hDs, 1);
            dt = GDALGetRasterDataType(hBand);

            const char *u = GDALGetRasterUnitType(hBand);
            if (u != nullptr) unit = u;

            // A pass over the band (or an overview): it stops when the call is canceled
            cancel::GdalProgress progress;
            double bMin, bMax, bMean, bStdDev;
            const CPLErr statsErr = GDALComputeRasterStatistics(hBand, TRUE, &bMin, &bMax, &bMean, &bStdDev,
                                                                progress.function(), progress.data());
            if (statsErr != CE_None)
                cancel::throwIfCanceled();
            if (statsErr == CE_None) {
                if (isThermal && dt == GDT_UInt16 && cal.valid) {
                    vMin = static_cast<float>(rawToTemperature(static_cast<uint16_t>(bMin), cal));
                    vMax = static_cast<float>(rawToTemperature(static_cast<uint16_t>(bMax), cal));
                    if (vMin > vMax) std::swap(vMin, vMax);
                } else {
                    vMin = static_cast<float>(bMin);
                    vMax = static_cast<float>(bMax);
                }
                hasStats = true;
            }
        }
    }

    // Fallback for small R-JPEG thermal images not fully handled by GDAL stats.
    if (!hasStats && isThermal) {
        TemperatureData td = readTemperatureData(filePath);
        if (!td.temperatures.empty()) {
            width = td.width;
            height = td.height;
            bandCount = 1;
            vMin = std::numeric_limits<float>::max();
            vMax = std::numeric_limits<float>::lowest();
            for (float t : td.temperatures) {
                if (std::isfinite(t) && t > -273.15f && t < 1000.0f) {
                    vMin = std::min(vMin, t);
                    vMax = std::max(vMax, t);
                }
            }
            hasStats = true;
        }
    }

    if (!hasStats) {
        throw AppException("Cannot read raster data from " + filePath);
    }

    // Provide a sensible default unit label for thermal rasters if not advertised by GDAL.
    if (unit.empty() && isThermal) unit = "\xC2\xB0\x43"; // UTF-8 "°C"

    json result;
    result["width"] = width;
    result["height"] = height;
    result["bandCount"] = bandCount;
    result["dataType"] = dataTypeName(dt);
    result["valueMin"] = vMin;
    result["valueMax"] = vMax;
    result["unit"] = unit;
    result["isThermal"] = isThermal;
    result["isDirectValue"] = isDirect;
    result["sensorId"] = sensorId;

    if (isThermal && cal.valid)
        result["calibration"] = calibrationJson(cal);

    return result.dump();
}

std::string getRasterPointJson(const std::string &filePath, int x, int y) {
    cancel::throwIfCanceled();
    std::string sensorId;
    const bool isThermal = detectIsThermal(filePath, sensorId);

    if (const auto dataset = raster::openInputRaster(filePath)) {
        GDALDatasetH hDs = datasetHandle(dataset);
        const int w = GDALGetRasterXSize(hDs);
        const int h = GDALGetRasterYSize(hDs);

        if (x < 0 || x >= w || y < 0 || y >= h) {
            throw InvalidArgsException("Pixel coordinates out of bounds: (" +
                std::to_string(x) + ", " + std::to_string(y) + ") for image " +
                std::to_string(w) + "x" + std::to_string(h));
        }

        if (GDALGetRasterCount(hDs) >= 1) {
            if (const auto pixel = readPixel(GDALGetRasterBand(hDs, 1), x, y, isThermal, filePath)) {
                json result = pointResult(*pixel, x, y, isThermal);
                double geoTransform[6];
                const char *proj = GDALGetProjectionRef(hDs);
                setGeoPosition(result, GDALGetGeoTransform(hDs, geoTransform) == CE_None, geoTransform,
                               proj != nullptr ? proj : "", x, y);
                return result.dump();
            }
        }
    }

    // Fallback: R-JPEG (small thermal images).
    if (!isThermal) {
        throw AppException("Cannot read raster data from " + filePath);
    }
    return thermalImagePointJson(filePath, x, y);
}

std::string getRasterAreaStatsJson(const std::string &filePath,
                                   int x0, int y0, int x1, int y1) {
    cancel::throwIfCanceled();
    std::string sensorId;
    const bool isThermal = detectIsThermal(filePath, sensorId);

    if (const auto dataset = raster::openInputRaster(filePath)) {
        GDALDatasetH hDs = datasetHandle(dataset);
        const int w = GDALGetRasterXSize(hDs);
        const int h = GDALGetRasterYSize(hDs);
        const int nBands = GDALGetRasterCount(hDs);

        const AreaBounds bounds = AreaBounds::clamped(x0, y0, x1, y1, w, h);

        std::string unit;

        if (nBands >= 1) {
            GDALRasterBandH hBand = GDALGetRasterBand(hDs, 1);
            GDALDataType dt = GDALGetRasterDataType(hBand);
            const char *u = GDALGetRasterUnitType(hBand);
            if (u != nullptr) unit = u;

            ThermalCalibration cal;
            bool needsConversion = false;
            if (isThermal && dt == GDT_UInt16) {
                cal = extractThermalCalibration(filePath);
                needsConversion = cal.valid;
            }

            if (const auto roiData = readAreaBand(hBand, dt, bounds, needsConversion, cal)) {
                // Respect nodata.
                const BandNodata noData = readBandNodata(hBand);

                std::vector<float> values;
                values.reserve(roiData->size());
                for (float v : *roiData) {
                    if (!std::isfinite(v)) continue;
                    if (isNodata(v, noData)) continue;
                    values.push_back(v);
                }

                if (values.empty()) {
                    throw AppException("No valid data in the specified area");
                }

                if (unit.empty() && isThermal) unit = "\xC2\xB0\x43";

                return areaStatsJson(std::move(values), bounds, unit, isThermal);
            }
        }
    }

    // Fallback: R-JPEG thermal only.
    if (!isThermal) {
        throw AppException("Cannot read raster data from " + filePath);
    }

    TemperatureData td = readTemperatureData(filePath);
    if (td.temperatures.empty()) {
        throw AppException("Cannot read raster data from " + filePath);
    }

    const AreaBounds bounds = AreaBounds::clamped(x0, y0, x1, y1, td.width, td.height);

    std::vector<float> values;
    values.reserve(static_cast<size_t>(bounds.x1 - bounds.x0 + 1) * (bounds.y1 - bounds.y0 + 1));
    for (int row = bounds.y0; row <= bounds.y1; row++) {
        for (int col = bounds.x0; col <= bounds.x1; col++) {
            float t = td.temperatures[static_cast<size_t>(row) * td.width + col];
            if (std::isfinite(t) && t > -273.15f && t < 1000.0f) {
                values.push_back(t);
            }
        }
    }

    if (values.empty()) {
        throw AppException("No valid data in the specified area");
    }

    return areaStatsJson(std::move(values), bounds, "\xC2\xB0\x43", true);
}

// ------------------------------------------------------------------ raster metadata

namespace {

// The statistics of the values of a formula (the NODATA_SENTINEL ones are no data), as the
// statistics of band "1": min, max, mean, std, p2 and p98, and a histogram of 255 bins. Null
// when no pixel has a value.
json formulaStatistics(const std::vector<float> &formulaResult) {
    std::vector<double> valid;
    valid.reserve(formulaResult.size());
    double sum = 0, sumSq = 0;
    double fMin = std::numeric_limits<double>::max();
    double fMax = std::numeric_limits<double>::lowest();
    for (const float f : formulaResult) {
        if (f != NODATA_SENTINEL) {
            double v = static_cast<double>(f);
            valid.push_back(v);
            sum += v;
            sumSq += v * v;
            fMin = std::min(fMin, v);
            fMax = std::max(fMax, v);
        }
    }
    if (valid.empty())
        return json();

    const double count = static_cast<double>(valid.size());
    double mean = sum / count;
    double variance = (sumSq / count) - (mean * mean);
    double stddev = std::sqrt(std::max(0.0, variance));

    std::sort(valid.begin(), valid.end());
    double p2 = valid[static_cast<size_t>(count * 0.02)];
    double p98 = valid[std::min(valid.size() - 1, static_cast<size_t>(count * 0.98))];

    // Histogram
    int bins = 255;
    std::vector<int> counts(bins, 0);
    std::vector<double> edges(bins + 1);
    double range = fMax - fMin;
    if (range <= 0) range = 1.0;
    for (int i = 0; i <= bins; i++) {
        edges[i] = fMin + (range * i / bins);
    }
    for (double v : valid) {
        int idx = static_cast<int>((v - fMin) / range * (bins - 1));
        idx = std::max(0, std::min(bins - 1, idx));
        counts[idx]++;
    }

    json stats;
    stats["1"] = {
        {"min", fMin}, {"max", fMax}, {"mean", mean}, {"std", stddev},
        {"percentiles", {{"p2", p2}, {"p98", p98}}},
        {"histogram", {{"counts", counts}, {"edges", edges}, {"bins", bins}}}
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
    return formulaStatistics(buffers.result);
}

// The statistics of every band GDAL can compute them for, by band number; null when none
json bandStatistics(GDALDatasetH hDs) {
    json stats;
    const int nBands = GDALGetRasterCount(hDs);
    for (int i = 1; i <= nBands; i++) {
        GDALRasterBandH hBand = GDALGetRasterBand(hDs, i);
        double bMin, bMax, bMean, bStdDev;
        // A pass over the band (or an overview): it stops when the call is canceled
        cancel::GdalProgress progress;
        const CPLErr statsErr = GDALComputeRasterStatistics(hBand, TRUE, &bMin, &bMax, &bMean, &bStdDev,
                                                            progress.function(), progress.data());
        if (statsErr != CE_None)
            cancel::throwIfCanceled();
        if (statsErr == CE_None) {
            stats[std::to_string(i)] = {
                {"min", bMin}, {"max", bMax}, {"mean", bMean}, {"std", bStdDev},
                {"percentiles", {{"p2", bMean - 2.33 * bStdDev}, {"p98", bMean + 2.33 * bStdDev}}}
            };
        }
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
                                            : bandStatistics(hDs);

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
