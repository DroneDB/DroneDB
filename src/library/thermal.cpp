/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "thermal.h"
#include "stringutil.h"
#include "exif.h"
#include "exceptions.h"
#include "coordstransformer.h"
#include "gdal_raii.h"
#include "logger.h"
#include "sensorprofile.h"
#include "mio.h"

#include <optional>
#include <gdal_priv.h>
#include <cpl_conv.h>
#include <ogr_spatialref.h>

#include <fstream>
#include <cmath>
#include <algorithm>
#include <numeric>

namespace ddb {

// ---- Thermal sensor detection by Make/Model ----

static const std::vector<std::pair<std::string, std::string>> thermalSensorPatterns = {
    {"FLIR", ""},           // Any FLIR camera
    {"DJI", "H20T"},
    {"DJI", "ZH20T"},
    {"DJI", "Zenmuse H20T"},
    {"DJI", "H30T"},
    {"DJI", "ZH30T"},
    {"DJI", "Zenmuse H30T"},
    {"DJI", "MAVIC3T"},
    {"DJI", "M3T"},
    {"DJI", "Mavic 3T"},
    {"DJI", "Mavic 3 Enterprise"},
    {"Workswell", "WirisProSc"},
    {"MicaSense", "Altum"},
};

bool isThermalImageFromExif(const std::string &make, const std::string &model) {
    std::string makeLower = make;
    std::string modelLower = model;
    str::toLower(makeLower);
    str::toLower(modelLower);

    for (const auto &pattern : thermalSensorPatterns) {
        std::string patMake = pattern.first;
        std::string patModel = pattern.second;
        str::toLower(patMake);
        str::toLower(patModel);

        if (makeLower.find(patMake) != std::string::npos) {
            if (patModel.empty()) return true;
            if (modelLower.find(patModel) != std::string::npos) return true;
        }
    }
    return false;
}

// ---- R-JPEG segment finding ----

// Find FLIR APP1 segment in a JPEG file.
// Returns the offset to the raw thermal data blob within the file, and its size.
struct FlirSegmentInfo {
    size_t rawDataOffset = 0;
    size_t rawDataSize = 0;
    int rawWidth = 0;
    int rawHeight = 0;
    bool found = false;
};

// The offset just after the "FLIR\0" magic of an APP1 segment, when the file has one
static std::optional<size_t> findFlirMagic(const std::vector<uint8_t> &data) {
    const size_t fileSize = data.size();

    // Search for FLIR FFF record marker within APP1 segments
    // Pattern: FF E1 (APP1 marker) ... "FLIR\0" magic
    const uint8_t flirMagic[] = { 'F', 'L', 'I', 'R', 0x00 };

    for (size_t i = 0; i < fileSize - 10; i++) {
        // Check for APP1 marker: 0xFF 0xE1
        if (data[i] == 0xFF && data[i + 1] == 0xE1) {
            // Get segment length (big-endian 2 bytes after marker)
            uint16_t segLen = (static_cast<uint16_t>(data[i + 2]) << 8) | data[i + 3];

            // Check for FLIR magic after length
            if (i + 4 + 5 <= fileSize &&
                std::memcmp(&data[i + 4], flirMagic, 5) == 0) {
                LOGD << "Found FLIR APP1 segment at offset " << i << ", length " << segLen;
                return i + 4 + 5; // After "FLIR\0"
            }
        }
    }
    return std::nullopt;
}

// The dimensions of the raw thermal image: from the FLIR XMP, else the largest of the common
// thermal camera resolutions that fits in the data after the FLIR header (remainingData bytes)
static void findRawDimensions(const std::string &filePath, size_t remainingData, FlirSegmentInfo &info) {
    // Try to extract width/height from XMP/EXIF first. A reader that cannot work (no metadata-util)
    // is not a problem of this file and propagates.
    if (const auto parser = readExifParser(io::fromUtf8(filePath))) {
        try {
            // FLIR stores raw dimensions in XMP
            auto rawWidthIt = parser->findXmpKey({"Xmp.FLIR.RawThermalImageWidth"});
            auto rawHeightIt = parser->findXmpKey({"Xmp.FLIR.RawThermalImageHeight"});

            if (rawWidthIt && rawHeightIt) {
                info.rawWidth = rawWidthIt->toInt64();
                info.rawHeight = rawHeightIt->toInt64();
            }
        } catch (const std::exception &e) {
            LOGD << "Could not read FLIR XMP dimensions: " << e.what();
        }
    }

    // If dimensions not found from XMP, try common sizes
    if (info.rawWidth == 0 || info.rawHeight == 0) {
        // Common thermal camera resolutions
        struct { int w; int h; } commonSizes[] = {
            {640, 512}, {320, 256}, {160, 120},
            {1280, 1024}, {384, 288}, {640, 480}
        };

        for (const auto &s : commonSizes) {
            size_t expectedSize = static_cast<size_t>(s.w) * s.h * 2; // UInt16
            if (remainingData >= expectedSize) {
                info.rawWidth = s.w;
                info.rawHeight = s.h;
                break;
            }
        }
    }
}

// Search for the raw thermal data blob: a contiguous block of rawWidth * rawHeight * 2 bytes
// near the beginning of the FLIR APP1 data area
static void locateRawData(const std::vector<uint8_t> &data, size_t flirSegmentStart, FlirSegmentInfo &info) {
    const size_t fileSize = data.size();
    size_t expectedRawSize = static_cast<size_t>(info.rawWidth) * info.rawHeight * 2;

    for (size_t searchPos = flirSegmentStart; searchPos + expectedRawSize <= fileSize; searchPos++) {
        // Heuristic: raw thermal data typically has values in a reasonable range
        // Check first few pixels to validate
        uint16_t firstPixel = data[searchPos] | (static_cast<uint16_t>(data[searchPos + 1]) << 8);
        uint16_t midPixel = data[searchPos + expectedRawSize / 2] |
                           (static_cast<uint16_t>(data[searchPos + expectedRawSize / 2 + 1]) << 8);

        // Typical raw thermal values are in range 5000-30000 for room temp scenes
        if (firstPixel > 2000 && firstPixel < 50000 &&
            midPixel > 2000 && midPixel < 50000) {
            info.rawDataOffset = searchPos;
            info.rawDataSize = expectedRawSize;
            info.found = true;
            LOGD << "Found raw thermal data at offset " << searchPos
                 << " (" << info.rawWidth << "x" << info.rawHeight << ")";
            break;
        }
    }
}

// Locates the raw UInt16 thermal block of a FLIR or DJI R-JPEG. The FFF record directory is not
// parsed: the segment after the magic and 3 index bytes holds the raw values directly (DJI) or
// FFF sub-segments, so the block is found by its size (from the XMP, or a common sensor size)
// and the range of its values.
static FlirSegmentInfo findFlirSegment(const std::string &filePath) {
    FlirSegmentInfo info;

    std::ifstream file(io::fromUtf8(filePath), std::ios::binary);
    if (!file.is_open()) return info;

    // Read entire file into memory for simpler parsing
    file.seekg(0, std::ios::end);
    size_t fileSize = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    if (fileSize < 20) return info;

    std::vector<uint8_t> data(fileSize);
    file.read(reinterpret_cast<char*>(data.data()), fileSize);
    file.close();

    const auto flirSegmentStart = findFlirMagic(data);
    if (!flirSegmentStart) {
        LOGD << "No FLIR APP1 segment found in " << filePath;
        return info;
    }

    // After FLIR magic, skip 3 bytes (segment index info)
    size_t pos = *flirSegmentStart + 3;

    // Parse FFF header
    if (pos + 64 > fileSize) return info;

    findRawDimensions(filePath, fileSize - pos, info);
    if (info.rawWidth == 0 || info.rawHeight == 0) return info;

    locateRawData(data, *flirSegmentStart, info);
    return info;
}

// ---- R-JPEG raw data extraction ----

RawThermalData extractRawThermalData(const std::string &filePath) {
    RawThermalData result;

    // Method 1: Try FLIR APP1 segment extraction (FLIR R-JPEG format)
    FlirSegmentInfo seg = findFlirSegment(filePath);
    if (seg.found) {
        std::ifstream file(io::fromUtf8(filePath), std::ios::binary);
        if (!file.is_open()) return result;

        file.seekg(seg.rawDataOffset);
        size_t pixelCount = static_cast<size_t>(seg.rawWidth) * seg.rawHeight;
        result.data.resize(pixelCount);
        file.read(reinterpret_cast<char*>(result.data.data()), pixelCount * 2);

        result.width = seg.rawWidth;
        result.height = seg.rawHeight;
        result.valid = true;

        LOGD << "Extracted " << pixelCount << " raw thermal pixels from " << filePath;
        return result;
    }

    // Method 2: GDAL fallback (DJI R-JPEG, thermal GeoTIFF, etc.)
    LOGD << "No FLIR segment found, trying GDAL fallback for " << filePath;

    const auto dataset = adoptDataset(GDALOpen(filePath.c_str(), GA_ReadOnly));
    if (!dataset) return result;
    GDALDatasetH hDs = datasetHandle(dataset);

    int w = GDALGetRasterXSize(hDs);
    int h = GDALGetRasterYSize(hDs);
    int nBands = GDALGetRasterCount(hDs);

    if (nBands >= 1) {
        GDALRasterBandH hBand = GDALGetRasterBand(hDs, 1);
        GDALDataType dt = GDALGetRasterDataType(hBand);
        size_t pixelCount = static_cast<size_t>(w) * h;

        if (dt == GDT_UInt16) {
            // Direct raw radiometric data (e.g., thermal GeoTIFF)
            result.data.resize(pixelCount);
            if (GDALRasterIO(hBand, GF_Read, 0, 0, w, h,
                             result.data.data(), w, h, GDT_UInt16, 0, 0) == CE_None) {
                result.width = w;
                result.height = h;
                result.valid = true;
                LOGD << "Extracted " << pixelCount << " raw thermal pixels via GDAL (UInt16)";
            }
        } else if (dt == GDT_Byte) {
            // 8-bit thermal image (e.g., DJI R-JPEG)
            // Read first band and cast to UInt16
            result.data.resize(pixelCount);
            if (GDALRasterIO(hBand, GF_Read, 0, 0, w, h,
                             result.data.data(), w, h, GDT_UInt16, 0, 0) == CE_None) {
                result.width = w;
                result.height = h;
                result.valid = true;
                LOGD << "Extracted " << pixelCount << " thermal pixels via GDAL (Byte->UInt16)";
            }
        }
    }

    return result;
}

// ---- Calibration extraction ----

ThermalCalibration extractThermalCalibration(const std::string &filePath) {
    ThermalCalibration cal;

    // A reader that cannot work (no metadata-util) is not a problem of this file and propagates
    const auto parserPtr = readExifParser(io::fromUtf8(filePath));
    if (!parserPtr) return cal;
    ExifParser &parser = *parserPtr;

    try {
        // Try FLIR XMP namespace first
        auto getXmpDouble = [&](const std::initializer_list<std::string> &keys, double defaultVal) -> double {
            auto it = parser.findXmpKey(keys);
            if (it) {
                try {
                    return std::stod(it->toString());
                } catch (const std::invalid_argument &) {
                    // Non-numeric XMP value; use default silently.
                } catch (const std::out_of_range &) {
                    // Numeric out of double range; use default silently.
                }
            }
            return defaultVal;
        };

        // FLIR XMP calibration data
        cal.planckR1 = getXmpDouble({"Xmp.FLIR.PlanckR1"}, cal.planckR1);
        cal.planckB = getXmpDouble({"Xmp.FLIR.PlanckB"}, cal.planckB);
        cal.planckF = getXmpDouble({"Xmp.FLIR.PlanckF"}, cal.planckF);
        cal.planckO = getXmpDouble({"Xmp.FLIR.PlanckO"}, cal.planckO);
        cal.planckR2 = getXmpDouble({"Xmp.FLIR.PlanckR2"}, cal.planckR2);
        cal.emissivity = getXmpDouble({"Xmp.FLIR.Emissivity"}, cal.emissivity);
        cal.objectDistance = getXmpDouble({"Xmp.FLIR.ObjectDistance", "Xmp.FLIR.SubjectDistance"}, cal.objectDistance);
        cal.reflectedApparentTemperature = getXmpDouble(
            {"Xmp.FLIR.ReflectedApparentTemperature"}, cal.reflectedApparentTemperature);
        cal.atmosphericTemperature = getXmpDouble(
            {"Xmp.FLIR.AtmosphericTemperature"}, cal.atmosphericTemperature);
        cal.relativeHumidity = getXmpDouble(
            {"Xmp.FLIR.RelativeHumidity"}, cal.relativeHumidity);
        cal.irWindowTemperature = getXmpDouble(
            {"Xmp.FLIR.IRWindowTemperature"}, cal.irWindowTemperature);
        cal.irWindowTransmission = getXmpDouble(
            {"Xmp.FLIR.IRWindowTransmission"}, cal.irWindowTransmission);

        // Check if we found any FLIR-specific data
        auto flirCheck = parser.findXmpKey({"Xmp.FLIR.PlanckR1", "Xmp.FLIR.Emissivity"});
        if (flirCheck) {
            cal.valid = true;
            LOGD << "Found FLIR XMP calibration data";
            return cal;
        }

        // Fallback: DJI thermal EXIF/XMP tags
        cal.emissivity = getXmpDouble(
            {"Xmp.drone-dji.ThermalObjectEmissivity"}, cal.emissivity);
        cal.objectDistance = getXmpDouble(
            {"Xmp.drone-dji.ThermalObjectDistance"}, cal.objectDistance);
        cal.reflectedApparentTemperature = getXmpDouble(
            {"Xmp.drone-dji.ThermalReflection"}, cal.reflectedApparentTemperature);
        cal.atmosphericTemperature = getXmpDouble(
            {"Xmp.drone-dji.ThermalAtmosphericTemperature"}, cal.atmosphericTemperature);

        auto djiCheck = parser.findXmpKey({"Xmp.drone-dji.ThermalObjectEmissivity",
                                           "Xmp.drone-dji.ThermalMeasureMode"});
        if (djiCheck) {
            cal.valid = true;
            LOGD << "Found DJI thermal XMP calibration data";
            return cal;
        }

        // If we detect it's a thermal image by Make/Model, use defaults
        std::string make = parser.extractMake();
        std::string model = parser.extractModel();
        if (isThermalImageFromExif(make, model)) {
            cal.valid = true;
            LOGD << "Using default thermal calibration for " << make << " " << model;
        }

    } catch (const std::exception &e) {
        LOGD << "Error reading thermal calibration: " << e.what();
    }

    return cal;
}

// ---- Planck formula ----

double rawToTemperature(uint16_t raw, const ThermalCalibration &cal) {
    // T = B / ln(R1 / (R2 * (raw + O)) + F) - 273.15
    double rawD = static_cast<double>(raw);
    double denom = cal.planckR2 * (rawD + cal.planckO);

    if (std::abs(denom) < 1e-30) return 0.0;

    double arg = cal.planckR1 / denom + cal.planckF;
    if (arg <= 0.0) return 0.0;

    double tempK = cal.planckB / std::log(arg);
    return tempK - 273.15;
}

std::vector<float> rawToTemperatureMap(const RawThermalData &raw, const ThermalCalibration &cal) {
    size_t pixelCount = raw.data.size();
    std::vector<float> temps(pixelCount);

    for (size_t i = 0; i < pixelCount; i++) {
        temps[i] = static_cast<float>(rawToTemperature(raw.data[i], cal));
    }

    return temps;
}

// ---- Direct temperature raster detection ----
//
// A "direct temperature raster" is a raster whose pixel values are already
// temperatures (in °C or K), as opposed to raw sensor counts that need
// radiometric calibration. In practice this is always a Float32/Float64
// single-band raster.
//
// We intentionally do NOT use a pure value-range heuristic: many single-band
// Float32 rasters (DEMs/DSMs/DTMs, NDVI derivatives, etc.) have values that
// overlap plausible temperature ranges and would otherwise be misclassified
// as thermal. Registry, in particular, feeds us the COG build artifact
// (`cog.tif`) whose filename carries no hint about the original product.
//
// Instead we delegate to isThermalImage() which performs explicit metadata
// checks (Methods 1-5): EXIF make/model, FLIR/DJI XMP, FLIR APP1, and GDAL
// metadata keywords / unit type / Landsat-style scale+offset. That function
// also explicitly rejects DEM/DSM/DTM via negative keywords.
bool isDirectTemperatureRaster(const std::string &filePath) {
    if (!isThermalImage(filePath)) return false;

    const auto dataset = adoptDataset(GDALOpen(filePath.c_str(), GA_ReadOnly));
    if (!dataset) return false;
    GDALDatasetH hDs = datasetHandle(dataset);

    bool isDirect = false;
    if (GDALGetRasterCount(hDs) == 1) {
        GDALRasterBandH hBand = GDALGetRasterBand(hDs, 1);
        GDALDataType dt = GDALGetRasterDataType(hBand);
        isDirect = (dt == GDT_Float32 || dt == GDT_Float64);
    }

    return isDirect;
}

// ---- Detection ----

namespace {

// Methods 1-3: the EXIF make and model, the FLIR XMP namespace, the DJI thermal tags
bool hasThermalExifOrXmp(const std::string &filePath) {
    // A reader that cannot work (no metadata-util) is not a problem of this file and propagates
    const auto parser = readExifParser(io::fromUtf8(filePath));
    if (!parser) return false;

    try {
        std::string make = parser->extractMake();
        std::string model = parser->extractModel();

        if (isThermalImageFromExif(make, model)) return true;

        // Method 2: Check for FLIR XMP namespace
        auto flirIt = parser->findXmpKey({"Xmp.FLIR.PlanckR1",
                                          "Xmp.FLIR.Emissivity",
                                          "Xmp.FLIR.ThermalMeasureMode"});
        if (flirIt) return true;

        // Method 3: Check for DJI thermal tags
        auto djiIt = parser->findXmpKey({"Xmp.drone-dji.ThermalMeasureMode",
                                         "Xmp.drone-dji.ThermalObjectEmissivity"});
        if (djiIt) return true;
    } catch (const std::exception &e) {
        LOGD << "isThermalImage: EXIF/XMP probing failed: " << e.what();
    }
    return false;
}

// What the GDAL metadata of a single-band raster says: every metadata string in lowercase, the
// unit, and the scale and offset
struct ThermalSignals {
    std::string allMeta;
    std::string unitStr;
    std::string unitLower;
    int hasScale = FALSE, hasOffset = FALSE;
    double scale = 0, offset = 0;
};

ThermalSignals readThermalSignals(GDALDatasetH hDs) {
    // Collect all metadata strings for keyword scanning
    std::vector<std::string> metaStrings;

    // Dataset-level metadata across all domains
    const CPLStringList mdDomains(GDALGetMetadataDomainList(hDs), TRUE);
    for (int d = 0; d < mdDomains.size(); d++) {
        CSLConstList md = GDALGetMetadata(hDs, mdDomains[d]);
        if (md) {
            for (int k = 0; md[k] != nullptr; k++)
                metaStrings.push_back(md[k]);
        }
    }

    // Band-level metadata and description
    GDALRasterBandH hBand = GDALGetRasterBand(hDs, 1);
    const char *bandDesc = GDALGetDescription(hBand);
    if (bandDesc && bandDesc[0]) metaStrings.push_back(bandDesc);

    CSLConstList bmd = GDALGetMetadata(hBand, nullptr);
    if (bmd) {
        for (int k = 0; bmd[k] != nullptr; k++)
            metaStrings.push_back(bmd[k]);
    }

    ThermalSignals signals;

    // Band unit info
    const char *unitType = GDALGetRasterUnitType(hBand);
    signals.unitStr = unitType ? unitType : "";

    // Scale/offset (e.g., Landsat Collection 2: scale=0.00341802, offset=149.0)
    signals.scale = GDALGetRasterScale(hBand, &signals.hasScale);
    signals.offset = GDALGetRasterOffset(hBand, &signals.hasOffset);

    // Build a single lowercase string for keyword search
    for (const auto &s : metaStrings) {
        signals.allMeta += s;
        signals.allMeta += ' ';
    }
    str::toLower(signals.allMeta);

    signals.unitLower = signals.unitStr;
    str::toLower(signals.unitLower);
    return signals;
}

// Whether the signals describe a thermal raster: a DEM's keywords and units rule it out, then
// thermal keywords, units and the Landsat scale/offset pattern make it one
bool looksThermal(const ThermalSignals &signals) {
    const std::string &allMeta = signals.allMeta;
    const std::string &unitStr = signals.unitStr;
    const std::string &unitLower = signals.unitLower;

    // DEM negative signals - reject early
    static const std::vector<std::string> demKeywords = {
        "dem", "dtm", "dsm", "elevation", "height"
    };
    static const std::vector<std::string> demUnits = {
        "m", "ft", "meter", "meters", "feet", "foot"
    };
    static const std::vector<std::string> demCrsKeywords = {
        "vertical", "geoid", "ellipsoid height"
    };

    for (const auto &kw : demKeywords) {
        if (allMeta.find(kw) != std::string::npos) {
            LOGD << "GDAL metadata contains DEM keyword '" << kw << "', not thermal";
            return false;
        }
    }
    for (const auto &u : demUnits) {
        if (unitLower == u) {
            LOGD << "Band unit '" << unitStr << "' indicates DEM, not thermal";
            return false;
        }
    }
    for (const auto &kw : demCrsKeywords) {
        if (allMeta.find(kw) != std::string::npos) {
            LOGD << "GDAL metadata contains vertical CRS keyword '" << kw << "', not thermal";
            return false;
        }
    }

    // Thermal positive signals
    static const std::vector<std::string> thermalKeywords = {
        "thermal", "temperature", "surface_temperature",
        "lst", "tir", "brightness_temperature", "bt", "lwir"
    };
    static const std::vector<std::string> thermalUnits = {
        "kelvin", "celsius", "degc", "degk",
        "\xc2\xb0""c", "\xc2\xb0""k"  // UTF-8 for °C, °K
    };

    for (const auto &kw : thermalKeywords) {
        if (allMeta.find(kw) != std::string::npos) {
            LOGD << "GDAL metadata contains thermal keyword '" << kw << "'";
            return true;
        }
    }
    for (const auto &u : thermalUnits) {
        if (unitLower.find(u) != std::string::npos) {
            LOGD << "Band unit '" << unitStr << "' indicates thermal";
            return true;
        }
    }
    if (unitLower == "k") {
        LOGD << "Band unit 'K' indicates Kelvin (thermal)";
        return true;
    }

    // Landsat-style thermal scale/offset pattern
    if (signals.hasScale && signals.hasOffset && signals.scale > 0.001 && signals.scale < 0.01 &&
        signals.offset > 100 && signals.offset < 200) {
        LOGD << "Scale/offset pattern (scale=" << signals.scale << ", offset=" << signals.offset
             << ") consistent with Landsat thermal encoding";
        return true;
    }
    return false;
}

// Method 5: GDAL metadata classification for labeled thermal rasters
// (e.g., Landsat surface temperature, processed thermal GeoTIFFs)
// Uses explicit metadata signals; avoids value-range heuristics to prevent
// false positives on DEMs or other single-band Float32 rasters.
bool isThermalByGdalMetadata(const std::string &filePath) {
    auto dataset = adoptDataset(GDALOpen(filePath.c_str(), GA_ReadOnly));
    if (!dataset) return false;
    GDALDatasetH hDs = datasetHandle(dataset);
    if (GDALGetRasterCount(hDs) != 1) return false;

    const ThermalSignals signals = readThermalSignals(hDs);
    dataset.reset();
    return looksThermal(signals);
}

} // anonymous namespace

bool isThermalImage(const std::string &filePath) {
    // Methods 1-3: EXIF Make/Model, FLIR XMP namespace, DJI thermal tags
    if (hasThermalExifOrXmp(filePath)) return true;

    // Method 4: Check for FLIR APP1 segment
    FlirSegmentInfo seg = findFlirSegment(filePath);
    if (seg.found) return true;

    return isThermalByGdalMetadata(filePath);
}

// ---- Helper: read temperature data from file (R-JPEG or GeoTIFF) ----
// (TemperatureData struct is declared in thermal.h so raster_analysis.cpp can use it.)

TemperatureData readTemperatureData(const std::string &filePath) {
    TemperatureData td;

    // Try as GeoTIFF first (direct temperature or GDAL-readable)
    if (const auto dataset = adoptDataset(GDALOpen(filePath.c_str(), GA_ReadOnly))) {
        GDALDatasetH hDs = datasetHandle(dataset);
        int nBands = GDALGetRasterCount(hDs);
        td.width = GDALGetRasterXSize(hDs);
        td.height = GDALGetRasterYSize(hDs);

        td.hasGeoTransform = (GDALGetGeoTransform(hDs, td.geoTransform) == CE_None);
        const char* proj = GDALGetProjectionRef(hDs);
        if (proj) td.projection = proj;

        if (nBands >= 1) {
            GDALRasterBandH hBand = GDALGetRasterBand(hDs, 1);
            GDALDataType dt = GDALGetRasterDataType(hBand);

            size_t pixCount = static_cast<size_t>(td.width) * td.height;
            td.temperatures.resize(pixCount);

            if (dt == GDT_Float32 || dt == GDT_Float64) {
                // Direct temperature values
                if (GDALRasterIO(hBand, GF_Read, 0, 0, td.width, td.height,
                                 td.temperatures.data(), td.width, td.height,
                                 GDT_Float32, 0, 0) == CE_None) {
                    return td;
                }
            } else if (dt == GDT_UInt16) {
                // Raw values needing Planck conversion
                std::vector<uint16_t> rawData(pixCount);
                if (GDALRasterIO(hBand, GF_Read, 0, 0, td.width, td.height,
                                 rawData.data(), td.width, td.height,
                                 GDT_UInt16, 0, 0) == CE_None) {
                    ThermalCalibration cal = extractThermalCalibration(filePath);
                    RawThermalData raw;
                    raw.data = std::move(rawData);
                    raw.width = td.width;
                    raw.height = td.height;
                    raw.valid = true;
                    if (cal.valid) {
                        td.temperatures = rawToTemperatureMap(raw, cal);
                    } else {
                        LOGD << "No valid thermal calibration for " << filePath
                             << ", returning raw UInt16 values as float";
                        td.temperatures.resize(pixCount);
                        for (size_t p = 0; p < pixCount; p++) {
                            td.temperatures[p] = static_cast<float>(raw.data[p]);
                        }
                    }
                    return td;
                }
            }
        }
    }

    // Try R-JPEG extraction
    RawThermalData raw = extractRawThermalData(filePath);
    if (raw.valid) {
        ThermalCalibration cal = extractThermalCalibration(filePath);
        if (cal.valid) {
            td.temperatures = rawToTemperatureMap(raw, cal);
        } else {
            LOGD << "No valid thermal calibration for " << filePath
                 << ", returning raw R-JPEG values as float";
            size_t pixCount = raw.data.size();
            td.temperatures.resize(pixCount);
            for (size_t p = 0; p < pixCount; p++) {
                td.temperatures[p] = static_cast<float>(raw.data[p]);
            }
        }
        td.width = raw.width;
        td.height = raw.height;
        return td;
    }

    return td;
}

// ---- Helper: convert pixel to geo coordinates ----
void pixelToGeo(double px, double py, const double geoTransform[6],
                const std::string &projection, double &geoX, double &geoY) {
    // Map coordinates are the result whenever the projection cannot be transformed to WGS84
    geoX = geoTransform[0] + geoTransform[1] * px + geoTransform[2] * py;
    geoY = geoTransform[3] + geoTransform[4] * px + geoTransform[5] * py;

    if (projection.empty())
        return;

    const SrsPtr hSrs = srsFromWkt(projection.c_str());
    const SrsPtr hWgs84 = srsFromEpsg(4326);

    if (hSrs && hWgs84) {
        const auto toWgs84 = CoordsTransformer::tryCreate(srsOf(hSrs), srsOf(hWgs84));
        if (toWgs84) {
            double x = geoX;
            double y = geoY;
            if (toWgs84->tryTransform(1, &x, &y)) {
                geoX = x;
                geoY = y;
            }
        }
    }
}



} // namespace ddb
