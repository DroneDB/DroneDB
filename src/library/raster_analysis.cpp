/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "raster_analysis.h"
#include "thermal.h"
#include "sensorprofile.h"
#include "exceptions.h"
#include "logger.h"

#include <gdal_priv.h>
#include <cpl_conv.h>

#include <algorithm>
#include <cmath>
#include <limits>
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
    } catch (...) { /* ignore, fall through */ }

    // Heuristic fallback (R-JPEG FLIR APP1 / DJI XMP / Landsat-style scale/offset).
    if (isThermalImage(filePath) || isDirectTemperatureRaster(filePath)) {
        return true;
    }

    return false;
}

} // anonymous namespace

std::string readGdalUnitType(const std::string &filePath) {
    GDALDatasetH hDs = GDALOpen(filePath.c_str(), GA_ReadOnly);
    if (!hDs) return std::string();

    std::string unit;
    if (GDALGetRasterCount(hDs) >= 1) {
        GDALRasterBandH hBand = GDALGetRasterBand(hDs, 1);
        const char *u = GDALGetRasterUnitType(hBand);
        if (u != nullptr) unit = u;
    }

    GDALClose(hDs);
    return unit;
}

std::string getRasterValueInfoJson(const std::string &filePath) {
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

    GDALDatasetH hDs = GDALOpen(filePath.c_str(), GA_ReadOnly);
    if (hDs) {
        width = GDALGetRasterXSize(hDs);
        height = GDALGetRasterYSize(hDs);
        bandCount = GDALGetRasterCount(hDs);

        if (bandCount >= 1) {
            GDALRasterBandH hBand = GDALGetRasterBand(hDs, 1);
            dt = GDALGetRasterDataType(hBand);

            const char *u = GDALGetRasterUnitType(hBand);
            if (u != nullptr) unit = u;

            double bMin, bMax, bMean, bStdDev;
            if (GDALComputeRasterStatistics(hBand, TRUE, &bMin, &bMax, &bMean, &bStdDev,
                                            nullptr, nullptr) == CE_None) {
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
        GDALClose(hDs);
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

    if (isThermal && cal.valid) {
        result["calibration"] = {
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

    return result.dump();
}

std::string getRasterPointJson(const std::string &filePath, int x, int y) {
    std::string sensorId;
    const bool isThermal = detectIsThermal(filePath, sensorId);

    GDALDatasetH hDs = GDALOpen(filePath.c_str(), GA_ReadOnly);
    if (hDs) {
        const int w = GDALGetRasterXSize(hDs);
        const int h = GDALGetRasterYSize(hDs);

        if (x < 0 || x >= w || y < 0 || y >= h) {
            GDALClose(hDs);
            throw InvalidArgsException("Pixel coordinates out of bounds: (" +
                std::to_string(x) + ", " + std::to_string(y) + ") for image " +
                std::to_string(w) + "x" + std::to_string(h));
        }

        int nBands = GDALGetRasterCount(hDs);
        if (nBands >= 1) {
            GDALRasterBandH hBand = GDALGetRasterBand(hDs, 1);
            GDALDataType dt = GDALGetRasterDataType(hBand);

            float value = 0;
            float rawValue = 0;
            bool hasRaw = false;
            bool gotValue = false;

            if (dt == GDT_Float32 || dt == GDT_Float64) {
                float pixVal;
                if (GDALRasterIO(hBand, GF_Read, x, y, 1, 1, &pixVal, 1, 1,
                                 GDT_Float32, 0, 0) == CE_None) {
                    value = pixVal;
                    rawValue = pixVal;
                    gotValue = true;
                }
            } else if (dt == GDT_UInt16) {
                uint16_t raw;
                if (GDALRasterIO(hBand, GF_Read, x, y, 1, 1, &raw, 1, 1,
                                 GDT_UInt16, 0, 0) == CE_None) {
                    rawValue = static_cast<float>(raw);
                    hasRaw = true;
                    if (isThermal) {
                        ThermalCalibration cal = extractThermalCalibration(filePath);
                        value = cal.valid
                            ? static_cast<float>(rawToTemperature(raw, cal))
                            : static_cast<float>(raw);
                    } else {
                        value = static_cast<float>(raw);
                    }
                    gotValue = true;
                }
            } else {
                // Other integer types: read as Float32 passthrough.
                float pixVal;
                if (GDALRasterIO(hBand, GF_Read, x, y, 1, 1, &pixVal, 1, 1,
                                 GDT_Float32, 0, 0) == CE_None) {
                    value = pixVal;
                    rawValue = pixVal;
                    gotValue = true;
                }
            }

            if (gotValue) {
                json result;
                result["value"] = value;
                result["rawValue"] = hasRaw ? rawValue : value;
                result["x"] = x;
                result["y"] = y;
                result["isThermal"] = isThermal;

                double geoTransform[6];
                if (GDALGetGeoTransform(hDs, geoTransform) == CE_None) {
                    const char *proj = GDALGetProjectionRef(hDs);
                    if (proj && std::string(proj).length() > 0) {
                        double geoX = 0, geoY = 0;
                        pixelToGeo(x + 0.5, y + 0.5, geoTransform,
                                   std::string(proj), geoX, geoY);
                        result["geoX"] = geoX;
                        result["geoY"] = geoY;
                        result["hasGeo"] = true;
                    } else {
                        result["hasGeo"] = false;
                    }
                } else {
                    result["hasGeo"] = false;
                }

                GDALClose(hDs);
                return result.dump();
            }
        }
        GDALClose(hDs);
    }

    // Fallback: R-JPEG (small thermal images).
    if (!isThermal) {
        throw AppException("Cannot read raster data from " + filePath);
    }

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

    if (td.hasGeoTransform && !td.projection.empty()) {
        double geoX = 0, geoY = 0;
        pixelToGeo(x + 0.5, y + 0.5, td.geoTransform, td.projection, geoX, geoY);
        result["geoX"] = geoX;
        result["geoY"] = geoY;
        result["hasGeo"] = true;
    } else {
        result["hasGeo"] = false;
    }

    return result.dump();
}

std::string getRasterAreaStatsJson(const std::string &filePath,
                                   int x0, int y0, int x1, int y1) {
    std::string sensorId;
    const bool isThermal = detectIsThermal(filePath, sensorId);

    GDALDatasetH hDs = GDALOpen(filePath.c_str(), GA_ReadOnly);
    if (hDs) {
        const int w = GDALGetRasterXSize(hDs);
        const int h = GDALGetRasterYSize(hDs);
        const int nBands = GDALGetRasterCount(hDs);

        x0 = std::max(0, std::min(x0, w - 1));
        y0 = std::max(0, std::min(y0, h - 1));
        x1 = std::max(0, std::min(x1, w - 1));
        y1 = std::max(0, std::min(y1, h - 1));
        if (x0 > x1) std::swap(x0, x1);
        if (y0 > y1) std::swap(y0, y1);

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

            const int roiW = x1 - x0 + 1;
            const int roiH = y1 - y0 + 1;
            const size_t roiCount = static_cast<size_t>(roiW) * roiH;
            std::vector<float> roiData(roiCount);

            bool gotData = false;
            if (dt == GDT_Float32 || dt == GDT_Float64) {
                gotData = (GDALRasterIO(hBand, GF_Read, x0, y0, roiW, roiH,
                    roiData.data(), roiW, roiH, GDT_Float32, 0, 0) == CE_None);
            } else if (dt == GDT_UInt16) {
                std::vector<uint16_t> rawRoi(roiCount);
                if (GDALRasterIO(hBand, GF_Read, x0, y0, roiW, roiH,
                                 rawRoi.data(), roiW, roiH,
                                 GDT_UInt16, 0, 0) == CE_None) {
                    for (size_t i = 0; i < roiCount; i++) {
                        roiData[i] = needsConversion
                            ? static_cast<float>(rawToTemperature(rawRoi[i], cal))
                            : static_cast<float>(rawRoi[i]);
                    }
                    gotData = true;
                }
            } else {
                gotData = (GDALRasterIO(hBand, GF_Read, x0, y0, roiW, roiH,
                    roiData.data(), roiW, roiH, GDT_Float32, 0, 0) == CE_None);
            }

            if (gotData) {
                // Respect nodata.
                int hasNoData = 0;
                double noData = GDALGetRasterNoDataValue(hBand, &hasNoData);

                std::vector<float> values;
                values.reserve(roiCount);
                for (float v : roiData) {
                    if (!std::isfinite(v)) continue;
                    if (hasNoData && std::abs(static_cast<double>(v) - noData) < 1e-6) continue;
                    values.push_back(v);
                }

                GDALClose(hDs);

                if (values.empty()) {
                    throw AppException("No valid data in the specified area");
                }

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

                if (unit.empty() && isThermal) unit = "\xC2\xB0\x43";

                json result;
                result["min"] = vMin;
                result["max"] = vMax;
                result["mean"] = mean;
                result["stddev"] = stddev;
                result["median"] = median;
                result["pixelCount"] = static_cast<int>(values.size());
                result["bounds"] = {
                    {"x0", x0}, {"y0", y0}, {"x1", x1}, {"y1", y1}
                };
                result["unit"] = unit;
                result["isThermal"] = isThermal;
                return result.dump();
            }
        }
        GDALClose(hDs);
    }

    // Fallback: R-JPEG thermal only.
    if (!isThermal) {
        throw AppException("Cannot read raster data from " + filePath);
    }

    TemperatureData td = readTemperatureData(filePath);
    if (td.temperatures.empty()) {
        throw AppException("Cannot read raster data from " + filePath);
    }

    x0 = std::max(0, std::min(x0, td.width - 1));
    y0 = std::max(0, std::min(y0, td.height - 1));
    x1 = std::max(0, std::min(x1, td.width - 1));
    y1 = std::max(0, std::min(y1, td.height - 1));
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);

    std::vector<float> values;
    values.reserve(static_cast<size_t>(x1 - x0 + 1) * (y1 - y0 + 1));
    for (int row = y0; row <= y1; row++) {
        for (int col = x0; col <= x1; col++) {
            float t = td.temperatures[static_cast<size_t>(row) * td.width + col];
            if (std::isfinite(t) && t > -273.15f && t < 1000.0f) {
                values.push_back(t);
            }
        }
    }

    if (values.empty()) {
        throw AppException("No valid data in the specified area");
    }

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
        {"x0", x0}, {"y0", y0}, {"x1", x1}, {"y1", y1}
    };
    result["unit"] = std::string("\xC2\xB0\x43");
    result["isThermal"] = true;
    return result.dump();
}

} // namespace ddb
