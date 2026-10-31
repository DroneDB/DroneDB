/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef THERMAL_H
#define THERMAL_H

#include <optional>
#include <string>
#include <vector>
#include <cstdint>
#include "json_fwd.h"

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

// R-JPEG raw thermal data extracted from FLIR APP1 segment
struct RawThermalData {
    std::vector<uint16_t> data;
    int width = 0;
    int height = 0;
    bool valid = false;
};

// Detection

// Whether the tags of an image say it is thermal: the make and model of its camera (EXIF), the
// FLIR XMP namespace or the thermal tags of DJI. The tags are read with metadata-util, which the
// host starts and the raster helper never does: the helper gets this answer in its request.
bool hasThermalTags(const std::string &filePath);

// Whether an image or a raster is thermal: its tags (hasThermalTags), the FLIR segment of an
// R-JPEG, or the GDAL metadata of a single-band raster (thermal keywords, a temperature unit, the
// scale and offset of Landsat). thermalTags is what hasThermalTags says of the file when the caller
// knows it: then no tag is read, and the FLIR segment is found by the sizes of the common thermal
// sensors (this is how the raster helper asks). Without it the tags are read here, on the host.
bool isThermalImage(const std::string &filePath, std::optional<bool> thermalTags = std::nullopt);
bool isThermalImageFromExif(const std::string &make, const std::string &model);

// Calibration extraction
ThermalCalibration extractThermalCalibration(const std::string &filePath);

// R-JPEG raw data extraction
RawThermalData extractRawThermalData(const std::string &filePath);

// Temperature conversion
double rawToTemperature(uint16_t raw, const ThermalCalibration &cal);
std::vector<float> rawToTemperatureMap(const RawThermalData &raw, const ThermalCalibration &cal);

// Check if a GeoTIFF contains direct temperature values (e.g., ODM output)
bool isDirectTemperatureRaster(const std::string &filePath);

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

TemperatureData readTemperatureData(const std::string &filePath);
void pixelToGeo(double px, double py, const double geoTransform[6],
                const std::string &projection, double &geoX, double &geoY);

} // namespace ddb

#endif // THERMAL_H
