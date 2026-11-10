/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef THERMAL_H
#define THERMAL_H

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include "json_fwd.h"

namespace ddb {

// The radiometric calibration of a thermal camera: the Planck constants that turn its raw counts into temperatures
// (rawToTemperature), and what the camera was set to for the measurement. Every field is one the file says (FLIR: the
// CameraInfo record of its FFF data, else its XMP tags), never a default: a calibration is valid only when the five
// Planck constants were read, and a measurement field the file does not have is empty (the raster API reports only
// the fields that are set). From the CameraInfo record, the temperatures are in degrees Celsius (the record has them in
// K), the distance in metres and the humidity a fraction (0 to 1); from the XMP, the Planck constants, the emissivity,
// the distance, the humidity (a fraction) and the window transmission. The XMP temperatures (Xmp.FLIR.
// ReflectedApparentTemperature, AtmosphericTemperature, IRWindowTemperature) are not read: no real file has them (the
// FLIR XMP of the Vue Pro R, Duo Pro R and Zenmuse XT, XT R and XT2 has none), so their unit is not known, and a term
// whose temperature is not read is left out (rawToTemperature).
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
    // The coefficients of the atmospheric transmission (CameraInfo record only)
    std::optional<double> atmosphericTransAlpha1;
    std::optional<double> atmosphericTransAlpha2;
    std::optional<double> atmosphericTransBeta1;
    std::optional<double> atmosphericTransBeta2;
    std::optional<double> atmosphericTransX;
    bool valid = false; // the Planck constants were read from the file: the raw counts are temperatures
};

// The most pixels the raw thermal image of an R-JPEG is read with (FLIR: the RawData record of its FFF data, int16u or a
// PNG image, and the raw block of an APP1 segment without FFF records; DJI: the APP3 segments, whose payloads may add
// up to twice this in bytes), checked on the size the file declares before anything is allocated for it: 4 MP
// (2048 x 2048). The largest radiometric sensors are 1280 x 1024 (FLIR A8580 and
// X8580, DJI Zenmuse H30T); 4 MP is over three times that and holds twice the side of a 1024 x 768 sensor (2048 x 1536).
// A larger declaration is no raw thermal image: a small file must not make a request allocate gigabytes (a PNG image
// declaring 10000 x 10000 pixels inflates from some 200 KB to 200 MB of rows), while at this bound the counts, the rows
// of a PNG image and the temperatures of a picture take some 32 MB.
constexpr std::size_t kMaxRawThermalPixels = static_cast<std::size_t>(2048) * 2048;

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
// a DJI picture only when it carries the raw thermal image of the camera (hasDjiRawThermal, or the RawData record of the
// FFF data of the FLIR cores DJI sold: Zenmuse XT, XT R, XT2), whatever its model, since the wide and zoom pictures of
// a thermal drone (M30T, M3T, M2EA, H20N, H20T, H30T) have the model of the thermal one; any other camera by its make
// and model (isThermalImageFromExif)
bool isThermalPicture(const std::string &make, const std::string &model, const std::string &filePath);

// The calibration of a FLIR file: the CameraInfo record of the FFF data of its FLIR APP1 segments (no tag reader is
// needed), else its FLIR XMP tags. DJI tags give measurement parameters, never a valid calibration: the raw counts of
// DJI cameras are not those of the Planck formula (see djiRawToTemperatureMap)
ThermalCalibration extractThermalCalibration(const std::string &filePath);

// R-JPEG raw data extraction. An R-JPEG is a JPEG that carries the raw sensor counts of a thermal camera: FLIR (the
// RawData record of the FFF data of its FLIR APP1 segments: int16u, or a 16-bit PNG image, in the byte order of the
// record, at the size of the raw image, which may be portrait and differ from the picture's) or DJI (the APP3
// segments). The raw data of a file that is not an R-JPEG are the UInt16 band of
// a raster GDAL reads (a thermal GeoTIFF) of at most kMaxRawThermalPixels pixels, read whole: a larger raster has no raw
// data here (its counts are read a window at a time, ThermalBandReader); the 8-bit colour bands of an R-JPEG are a
// preview, never raw data.
RawThermalData extractRawThermalData(const std::string &filePath);

// The raw thermal data of an R-JPEG alone (FLIR or DJI), nothing for any other file: GDAL is not used
RawThermalData extractRJpegRawThermal(const std::string &filePath);

// The DJI raw-thermal probe (FR-RAS-13): whether the JPEG carries the raw thermal image of a DJI camera in its APP3
// segments, whatever its model (the rule of the GDAL JPEG driver: the segments hold width * height * 2 bytes, or
// the 640 x 512 of the nominal sensor when the picture has another size). Detection and reading agree because
// extractRJpegRawThermal reads the same segments. Only the header of the file is read.
bool hasDjiRawThermal(const std::string &filePath);

// Temperature conversion (Planck, FLIR): degrees Celsius, NaN for a raw count the calibration cannot convert (no
// temperature: the pixel has no value). The measurement parameters of the calibration are applied as the FLIR software
// applies them (Thermimage's raw2temp): the emissivity with the reflected apparent temperature, the transmission of the
// atmosphere (object distance, atmospheric temperature, relative humidity and the five coefficients) and the IR window
// (transmission and temperature). A parameter the file does not say leaves its term out: with the Planck constants
// alone, T = B / ln(R1 / (R2 (raw + O)) + F) - 273.15.
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

// The first band of a thermal raster GDAL reads, read a window at a time, so that what a request holds does not grow
// with the raster (a 30000 x 30000 thermal GeoTIFF read whole took some 9 GB): the point reads its pixel, the area and
// the value info stream their pixels (raster_analysis.cpp). The band holds temperatures (Float32 or Float64, read as
// Float32) or raw counts (UInt16): with the Planck constants of the file (extractThermalCalibration) the counts are
// turned into degrees Celsius as rawToTemperatureMap turns them, else they are taken as they are. Any other band is not
// read.
class ThermalBandReader {
public:
    // The reader of the first band of the raster; nothing when GDAL cannot open it, it has no band, or its first band
    // is neither Float32, Float64 nor UInt16
    static std::unique_ptr<ThermalBandReader> open(const std::string &filePath);
    ~ThermalBandReader();
    ThermalBandReader(const ThermalBandReader &) = delete;
    ThermalBandReader &operator=(const ThermalBandReader &) = delete;

    const std::string &path() const;
    int width() const;
    int height() const;
    // The height of a block of the band (the rows a read is best aligned on)
    int blockHeight() const;
    bool hasGeoTransform() const;
    const double *geoTransform() const;
    const std::string &projection() const;
    // Whether the band holds raw counts (UInt16)
    bool hasRawCounts() const;

    // The temperatures of the w x h window at (x, y), row by row, and its raw counts when the band has them (empty
    // otherwise); false when GDAL cannot read it
    bool read(int x, int y, int w, int h, std::vector<float> &temperatures, std::vector<uint16_t> &rawCounts) const;

private:
    struct Impl;
    explicit ThermalBandReader(std::unique_ptr<Impl> state);
    std::unique_ptr<Impl> impl;
};

// The temperatures of an R-JPEG, as the raster API reads them (FR-RAS-14): the raw counts of the camera, and their
// temperatures when they can be told. Nothing for a file that is not an R-JPEG. grid.temperatures (degrees Celsius, one
// per raw count, grid.width x grid.height) is empty when no conversion of the raw counts is known: the file is an
// R-JPEG, its values are raw counts without a unit. FLIR: the Planck formula with the calibration of its CameraInfo
// record, else of its XMP tags (a FLIR file without the FFF records needs the metadata reader, which the raster helper
// does not have: it throws there). DJI: DJI's own conversion (djiRawToTemperatureMap), which is not available: raw
// counts, no calibration (dji is true).
struct RJpegThermal {
    RawThermalData raw;
    ThermalCalibration calibration;
    TemperatureData grid;
    bool dji = false; // the raw counts of a DJI camera

    bool hasTemperatures() const { return !grid.temperatures.empty(); }
};
std::optional<RJpegThermal> readRJpegThermal(const std::string &filePath);
void pixelToGeo(double px, double py, const double geoTransform[6],
                const std::string &projection, double &geoX, double &geoY);

// Temperature units (D-022, FR-XC-24, FR-RAS-03). The one place that knows what the unit of a band is and
// how a thermal formula (CELSIUS, KELVIN, FAHRENHEIT) turns the values of a band into the unit it is named
// after: the point, area, profile, contour, value info, tile, thumbnail and export of a thermal raster all
// convert here, never with a constant of their own. New in 2.4 (the FAHRENHEIT formula and the conversion
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
