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

// The radiometric calibration of a thermal camera: the Planck constants that turn its raw counts into temperatures
// (rawToTemperature), and what the camera was set to for the measurement. Every field is one the file says (FLIR: its
// XMP tags), never a default: a calibration is valid only when the five Planck constants were read, and a measurement
// field the file does not have is empty (the raster API reports only the fields that are set).
struct ThermalCalibration {
    double planckR1 = 0.0;
    double planckB = 0.0;
    double planckF = 0.0;
    double planckO = 0.0;
    double planckR2 = 0.0;
    std::optional<double> emissivity;
    std::optional<double> objectDistance;
    std::optional<double> reflectedApparentTemperature;
    std::optional<double> atmosphericTemperature;
    std::optional<double> relativeHumidity;
    std::optional<double> irWindowTemperature;
    std::optional<double> irWindowTransmission;
    bool valid = false; // the Planck constants were read from the file: the raw counts are temperatures
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

// Whether a picture is one of a thermal camera, as the scan of an entry marks it (sensorCategory "thermal", FR-RAS-13):
// a DJI picture only when it carries the raw thermal image of the camera (hasDjiRawThermal), whatever its model, since
// the wide and zoom pictures of a thermal drone (M30T, M3T, M2EA, H20N, H20T, H30T) have the model of the thermal one;
// any other camera by its make and model (isThermalImageFromExif)
bool isThermalPicture(const std::string &make, const std::string &model, const std::string &filePath);

// The calibration the tags of the file give (FLIR XMP). DJI tags give measurement parameters, never a valid calibration:
// the raw counts of DJI cameras are not those of the Planck formula (see djiRawToTemperatureMap)
ThermalCalibration extractThermalCalibration(const std::string &filePath);

// R-JPEG raw data extraction. An R-JPEG is a JPEG that carries the raw sensor counts of a thermal camera: FLIR (the
// FLIR APP1 segment) or DJI (the APP3 segments). The raw data of a file that is not an R-JPEG are the UInt16 band of
// a raster GDAL reads (a thermal GeoTIFF); the 8-bit colour bands of an R-JPEG are a preview, never raw data.
RawThermalData extractRawThermalData(const std::string &filePath);

// The raw thermal data of an R-JPEG alone (FLIR or DJI), nothing for any other file: GDAL is not used
RawThermalData extractRJpegRawThermal(const std::string &filePath);

// The DJI raw-thermal probe (FR-RAS-13): whether the JPEG carries the raw thermal image of a DJI camera in its APP3
// segments, whatever its model (the rule of the GDAL JPEG driver: the segments hold width * height * 2 bytes, or
// the 640 x 512 of the nominal sensor when the picture has another size). Detection and reading agree because
// extractRJpegRawThermal reads the same segments. Only the header of the file is read.
bool hasDjiRawThermal(const std::string &filePath);

// Temperature conversion (Planck, FLIR): degrees Celsius, NaN for a raw count the calibration cannot convert (no
// temperature: the pixel has no value)
double rawToTemperature(uint16_t raw, const ThermalCalibration &cal);
std::vector<float> rawToTemperatureMap(const RawThermalData &raw, const ThermalCalibration &cal);

// The extension point of DJI's own conversion of the raw counts of a DJI R-JPEG into degrees Celsius (dji_irp of the DJI
// Thermal SDK). The raw counts of DJI cameras are not those of the FLIR Planck formula: their conversion depends on the
// camera and on the measurement parameters of the file (APP4), and only DJI's conversion gives the temperatures the DJI
// software shows. It is not available (the SDK is not distributed with DroneDB, its license does not allow it), so this
// gives nothing and the raw counts of a DJI R-JPEG stay raw counts without a unit (FR-RAS-14). A conversion put here must
// give the temperatures of the DJI Thermal SDK samples (M2EA, H20T, H20N) as dji_irp gives them.
std::optional<std::vector<float>> djiRawToTemperatureMap(const RawThermalData &raw, const std::string &filePath);

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

// The temperatures of an R-JPEG, as the raster API reads them (FR-RAS-14): the raw counts of the camera, and their
// temperatures when they can be told. Nothing for a file that is not an R-JPEG. grid.temperatures (degrees Celsius, one
// per raw count, grid.width x grid.height) is empty when no conversion of the raw counts is known: the file is an
// R-JPEG, its values are raw counts without a unit. FLIR: the Planck formula with the calibration of its tags (a
// RawThermalData of a FLIR file needs the metadata reader, which the raster helper does not have: it throws there).
// DJI: DJI's own conversion (djiRawToTemperatureMap), which is not available: raw counts, no calibration.
struct RJpegThermal {
    RawThermalData raw;
    ThermalCalibration calibration;
    TemperatureData grid;

    bool hasTemperatures() const { return !grid.temperatures.empty(); }
};
std::optional<RJpegThermal> readRJpegThermal(const std::string &filePath);
void pixelToGeo(double px, double py, const double geoTransform[6],
                const std::string &projection, double &geoX, double &geoY);

// Temperature units (D-022, FR-XC-24, FR-RAS-03). The one place that knows what the unit of a band is and
// how a thermal formula (CELSIUS, KELVIN, FAHRENHEIT) turns the values of a band into the unit it is named
// after: the point, area, profile, contour, value info, tile, thumbnail and export of a thermal raster all
// convert here, never with a constant of their own. New in 2.3 (the FAHRENHEIT formula and the conversion
// of every value endpoint).
enum class TemperatureUnit { Celsius, Kelvin, Fahrenheit };

// The unit a unit type of a band names: "K", "Kelvin", "degK", "\u00B0K" -> Kelvin; "Celsius", "degC",
// "\u00B0C", "C" -> Celsius; "F", "Fahrenheit", "degF", "\u00B0F" -> Fahrenheit (any case, no space or
// punctuation); nothing for any other text
std::optional<TemperatureUnit> parseTemperatureUnit(const std::string &unit);

// The unit of the values of a band: its unit type when it names a temperature unit, else degrees Celsius
// (the unit of a thermal raster that does not say it)
TemperatureUnit bandTemperatureUnit(const std::string &bandUnit);

// The symbol of a unit, in UTF-8: "\u00B0C", "K", "\u00B0F"
const char *temperatureUnitSymbol(TemperatureUnit unit);

// The unit a thermal formula gives: CELSIUS, KELVIN or FAHRENHEIT (exact id); nothing for any other id
std::optional<TemperatureUnit> thermalFormulaUnit(const std::string &formulaId);

// value in the target unit = value * scale + offset
struct TemperatureConversion {
    double scale = 1.0;
    double offset = 0.0;

    double apply(double value) const { return value * scale + offset; }
    bool isIdentity() const { return scale == 1.0 && offset == 0.0; }
};

TemperatureConversion temperatureConversion(TemperatureUnit from, TemperatureUnit to);

// What a thermal formula does to the values of a band whose unit type is bandUnit: the conversion and the unit
// ("\u00B0C", "K" or "\u00B0F") the converted values are in. A RefusedArgumentException ("Unknown formula: X")
// for a formula that does not exist, and for one that is not a thermal one (an index has no value at a point).
struct ThermalFormulaConversion {
    TemperatureConversion conversion;
    std::string unit;
};

ThermalFormulaConversion thermalFormulaConversion(const std::string &bandUnit, const std::string &formulaId);

// The same for values that are known to be degrees Celsius (the temperatures of a calibrated or R-JPEG grid)
ThermalFormulaConversion celsiusFormulaConversion(const std::string &formulaId);

// Whether a formula id is a thermal formula (exact id)
inline bool isThermalFormulaId(const std::string &formulaId) { return thermalFormulaUnit(formulaId).has_value(); }

} // namespace ddb

#endif // THERMAL_H
