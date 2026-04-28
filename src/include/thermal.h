/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef THERMAL_H
#define THERMAL_H

#include <string>
#include <vector>
#include <cstdint>
#include "ddb_export.h"
#include "json.h"

namespace ddb {

struct ThermalCalibration {
    double planckR1 = 21106.77;
    double planckB = 1501.0;
    double planckF = 1.0;
    double planckO = -7340.0;
    double planckR2 = 0.012545258;
    double emissivity = 0.95;
    double objectDistance = 5.0;
    double reflectedApparentTemperature = 20.0;
    double atmosphericTemperature = 20.0;
    double relativeHumidity = 50.0;
    double irWindowTemperature = 20.0;
    double irWindowTransmission = 1.0;
    bool valid = false;
};

struct ThermalInfo {
    ThermalCalibration calibration;
    double temperatureMin = 0.0;
    double temperatureMax = 0.0;
    bool isDirectTemperature = false;
    std::string sensorId;
    int width = 0;
    int height = 0;
};

struct ThermalPoint {
    double temperature = 0.0;
    double rawValue = 0.0;
    int x = 0;
    int y = 0;
    double geoX = 0.0;
    double geoY = 0.0;
    bool hasGeo = false;
};

struct ThermalAreaStats {
    double min = 0.0;
    double max = 0.0;
    double mean = 0.0;
    double stddev = 0.0;
    double median = 0.0;
    int pixelCount = 0;
};

// R-JPEG raw thermal data extracted from FLIR APP1 segment
struct RawThermalData {
    std::vector<uint16_t> data;
    int width = 0;
    int height = 0;
    bool valid = false;
};

// Detection
DDB_DLL bool isThermalImage(const std::string &filePath);
DDB_DLL bool isThermalImageFromExif(const std::string &make, const std::string &model);

// Calibration extraction
DDB_DLL ThermalCalibration extractThermalCalibration(const std::string &filePath);

// R-JPEG raw data extraction
DDB_DLL RawThermalData extractRawThermalData(const std::string &filePath);

// Temperature conversion
DDB_DLL double rawToTemperature(uint16_t raw, const ThermalCalibration &cal);
DDB_DLL std::vector<float> rawToTemperatureMap(const RawThermalData &raw, const ThermalCalibration &cal);

// Check if a GeoTIFF contains direct temperature values (e.g., ODM output)
DDB_DLL bool isDirectTemperatureRaster(const std::string &filePath);

// Internal shared helpers used by raster_analysis.cpp.
// They live here so that thermal-specific code (Planck calibration, R-JPEG extraction)
// can be reused by the neutral raster analysis surface without duplication.
struct TemperatureData {
    std::vector<float> temperatures;
    int width = 0;
    int height = 0;
    bool hasGeoTransform = false;
    double geoTransform[6] = {};
    std::string projection;
};

DDB_DLL TemperatureData readTemperatureData(const std::string &filePath);
DDB_DLL void pixelToGeo(double px, double py, const double geoTransform[6],
                        const std::string &projection, double &geoX, double &geoY);

} // namespace ddb

#endif // THERMAL_H
