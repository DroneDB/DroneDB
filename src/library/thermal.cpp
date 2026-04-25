/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "thermal.h"
#include "exif.h"
#include "exceptions.h"
#include "logger.h"
#include "sensorprofile.h"

#include <exiv2/exiv2.hpp>
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
    std::transform(makeLower.begin(), makeLower.end(), makeLower.begin(), ::tolower);
    std::transform(modelLower.begin(), modelLower.end(), modelLower.begin(), ::tolower);

    for (const auto &pattern : thermalSensorPatterns) {
        std::string patMake = pattern.first;
        std::string patModel = pattern.second;
        std::transform(patMake.begin(), patMake.end(), patMake.begin(), ::tolower);
        std::transform(patModel.begin(), patModel.end(), patModel.begin(), ::tolower);

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

static FlirSegmentInfo findFlirSegment(const std::string &filePath) {
    FlirSegmentInfo info;

    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) return info;

    // Read entire file into memory for simpler parsing
    file.seekg(0, std::ios::end);
    size_t fileSize = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    if (fileSize < 20) return info;

    std::vector<uint8_t> data(fileSize);
    file.read(reinterpret_cast<char*>(data.data()), fileSize);
    file.close();

    // Search for FLIR FFF record marker within APP1 segments
    // Pattern: FF E1 (APP1 marker) ... "FLIR\0" magic
    const uint8_t flirMagic[] = { 'F', 'L', 'I', 'R', 0x00 };

    size_t flirSegmentStart = 0;
    bool foundMagic = false;

    for (size_t i = 0; i < fileSize - 10; i++) {
        // Check for APP1 marker: 0xFF 0xE1
        if (data[i] == 0xFF && data[i + 1] == 0xE1) {
            // Get segment length (big-endian 2 bytes after marker)
            uint16_t segLen = (static_cast<uint16_t>(data[i + 2]) << 8) | data[i + 3];

            // Check for FLIR magic after length
            if (i + 4 + 5 <= fileSize &&
                std::memcmp(&data[i + 4], flirMagic, 5) == 0) {
                flirSegmentStart = i + 4 + 5; // After "FLIR\0"
                foundMagic = true;
                LOGD << "Found FLIR APP1 segment at offset " << i << ", length " << segLen;
                break;
            }
        }
    }

    if (!foundMagic) {
        LOGD << "No FLIR APP1 segment found in " << filePath;
        return info;
    }

    // After FLIR magic, skip 3 bytes (segment index info)
    // Then look for FFF record directory
    size_t pos = flirSegmentStart + 3;

    // Search for raw thermal data record type (0x0001 = RawData)
    // The FFF record directory has entries of 32 bytes each
    // Record type at offset 0 (uint16 BE), followed by various fields
    // We look for the raw data embedded in the FLIR segment

    // Simplified approach: scan for patterns that indicate raw thermal data
    // In FLIR FFF format, raw thermal data is typically UInt16 Little-Endian
    // preceded by width/height metadata

    // Look for a secondary FLIR marker or raw data indicator
    // The typical FLIR R-JPEG has the raw data at a specific offset
    // encoded in the FFF record directory

    // Parse FFF header
    if (pos + 64 > fileSize) return info;

    // Skip to raw data record — in most FLIR R-JPEGs, the raw data
    // follows a recognizable pattern. We search for the raw data record.

    // FLIR FFF Record Directory parsing:
    // After the FLIR magic + 3 bytes, there are records.
    // Each record: type(2) + ... + dataOffset(4) + dataLength(4) at specific positions

    // Actually, the FLIR segment after magic+3 contains:
    // [1 byte: segment sequence number]  - already skipped as part of +3
    // Then: direct raw thermal data or FFF sub-segments

    // For DJI thermal images, the structure is simpler:
    // The thermal data is embedded directly as raw UInt16 values
    // We need to find resolution from EXIF and locate raw block

    // Try to extract width/height from XMP/EXIF first
    try {
        auto exivImage = Exiv2::ImageFactory::open(filePath);
        if (exivImage.get()) {
            exivImage->readMetadata();
            ExifParser parser(exivImage.get());

            // FLIR stores raw dimensions in XMP
            auto rawWidthIt = parser.findXmpKey({"Xmp.FLIR.RawThermalImageWidth"});
            auto rawHeightIt = parser.findXmpKey({"Xmp.FLIR.RawThermalImageHeight"});

            if (rawWidthIt != parser.xmpEnd() && rawHeightIt != parser.xmpEnd()) {
                info.rawWidth = rawWidthIt->toInt64();
                info.rawHeight = rawHeightIt->toInt64();
            }
        }
    } catch (...) {
        LOGD << "Could not read FLIR XMP dimensions";
    }

    // If dimensions not found from XMP, try common sizes
    if (info.rawWidth == 0 || info.rawHeight == 0) {
        // Common thermal camera resolutions
        struct { int w; int h; } commonSizes[] = {
            {640, 512}, {320, 256}, {160, 120},
            {1280, 1024}, {384, 288}, {640, 480}
        };

        size_t remainingData = fileSize - pos;
        for (const auto &s : commonSizes) {
            size_t expectedSize = static_cast<size_t>(s.w) * s.h * 2; // UInt16
            if (remainingData >= expectedSize) {
                info.rawWidth = s.w;
                info.rawHeight = s.h;
                break;
            }
        }
    }

    if (info.rawWidth == 0 || info.rawHeight == 0) return info;

    // Search for the raw thermal data blob
    // It's typically a contiguous block of rawWidth * rawHeight * 2 bytes
    size_t expectedRawSize = static_cast<size_t>(info.rawWidth) * info.rawHeight * 2;

    // Scan the FLIR segment area for the raw data
    // The raw data is usually near the beginning of the FLIR segment
    // Look in the FLIR APP1 data area
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

    return info;
}

// ---- R-JPEG raw data extraction ----

RawThermalData extractRawThermalData(const std::string &filePath) {
    RawThermalData result;

    // Method 1: Try FLIR APP1 segment extraction (FLIR R-JPEG format)
    FlirSegmentInfo seg = findFlirSegment(filePath);
    if (seg.found) {
        std::ifstream file(filePath, std::ios::binary);
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

    GDALDatasetH hDs = GDALOpen(filePath.c_str(), GA_ReadOnly);
    if (!hDs) return result;

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

    GDALClose(hDs);
    return result;
}

// ---- Calibration extraction ----

ThermalCalibration extractThermalCalibration(const std::string &filePath) {
    ThermalCalibration cal;

    try {
        auto exivImage = Exiv2::ImageFactory::open(filePath);
        if (!exivImage.get()) return cal;
        exivImage->readMetadata();
        ExifParser parser(exivImage.get());

        // Try FLIR XMP namespace first
        auto getXmpDouble = [&](const std::initializer_list<std::string> &keys, double defaultVal) -> double {
            auto it = parser.findXmpKey(keys);
            if (it != parser.xmpEnd()) {
                try {
                    return std::stod(it->toString());
                } catch (...) {}
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
        if (flirCheck != parser.xmpEnd()) {
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
        if (djiCheck != parser.xmpEnd()) {
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

    } catch (const Exiv2::Error &e) {
        LOGD << "Exiv2 error reading thermal calibration: " << e.what();
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

    GDALDatasetH hDs = GDALOpen(filePath.c_str(), GA_ReadOnly);
    if (!hDs) return false;

    bool isDirect = false;
    if (GDALGetRasterCount(hDs) == 1) {
        GDALRasterBandH hBand = GDALGetRasterBand(hDs, 1);
        GDALDataType dt = GDALGetRasterDataType(hBand);
        isDirect = (dt == GDT_Float32 || dt == GDT_Float64);
    }

    GDALClose(hDs);
    return isDirect;
}

// ---- Detection ----

bool isThermalImage(const std::string &filePath) {
    // Method 1: Check EXIF Make/Model
    try {
        auto exivImage = Exiv2::ImageFactory::open(filePath);
        if (exivImage.get()) {
            exivImage->readMetadata();
            ExifParser parser(exivImage.get());

            std::string make = parser.extractMake();
            std::string model = parser.extractModel();

            if (isThermalImageFromExif(make, model)) return true;

            // Method 2: Check for FLIR XMP namespace
            auto flirIt = parser.findXmpKey({"Xmp.FLIR.PlanckR1",
                                             "Xmp.FLIR.Emissivity",
                                             "Xmp.FLIR.ThermalMeasureMode"});
            if (flirIt != parser.xmpEnd()) return true;

            // Method 3: Check for DJI thermal tags
            auto djiIt = parser.findXmpKey({"Xmp.drone-dji.ThermalMeasureMode",
                                            "Xmp.drone-dji.ThermalObjectEmissivity"});
            if (djiIt != parser.xmpEnd()) return true;
        }
    } catch (...) {}

    // Method 4: Check for FLIR APP1 segment
    FlirSegmentInfo seg = findFlirSegment(filePath);
    if (seg.found) return true;

    // Method 5: GDAL metadata classification for labeled thermal rasters
    // (e.g., Landsat surface temperature, processed thermal GeoTIFFs)
    // Uses explicit metadata signals; avoids value-range heuristics to prevent
    // false positives on DEMs or other single-band Float32 rasters.
    {
        GDALDatasetH hDs = GDALOpen(filePath.c_str(), GA_ReadOnly);
        if (hDs) {
            int nBands = GDALGetRasterCount(hDs);
            if (nBands == 1) {
                // Collect all metadata strings for keyword scanning
                std::vector<std::string> metaStrings;

                // Dataset-level metadata across all domains
                char **mdDomains = GDALGetMetadataDomainList(hDs);
                if (mdDomains) {
                    for (int d = 0; mdDomains[d] != nullptr; d++) {
                        char **md = GDALGetMetadata(hDs, mdDomains[d]);
                        if (md) {
                            for (int k = 0; md[k] != nullptr; k++)
                                metaStrings.push_back(md[k]);
                        }
                    }
                    CSLDestroy(mdDomains);
                }

                // Band-level metadata and description
                GDALRasterBandH hBand = GDALGetRasterBand(hDs, 1);
                const char *bandDesc = GDALGetDescription(hBand);
                if (bandDesc && bandDesc[0]) metaStrings.push_back(bandDesc);

                char **bmd = GDALGetMetadata(hBand, nullptr);
                if (bmd) {
                    for (int k = 0; bmd[k] != nullptr; k++)
                        metaStrings.push_back(bmd[k]);
                }

                // Band unit info
                const char *unitType = GDALGetRasterUnitType(hBand);
                std::string unitStr = unitType ? unitType : "";

                // Scale/offset (e.g., Landsat Collection 2: scale=0.00341802, offset=149.0)
                int hasScale = FALSE, hasOffset = FALSE;
                double scale = GDALGetRasterScale(hBand, &hasScale);
                double offset = GDALGetRasterOffset(hBand, &hasOffset);

                GDALClose(hDs);

                // Build a single lowercase string for keyword search
                std::string allMeta;
                for (const auto &s : metaStrings) {
                    allMeta += s;
                    allMeta += ' ';
                }
                std::transform(allMeta.begin(), allMeta.end(), allMeta.begin(), ::tolower);

                std::string unitLower = unitStr;
                std::transform(unitLower.begin(), unitLower.end(), unitLower.begin(), ::tolower);

                // DEM negative signals — reject early
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
                if (hasScale && hasOffset && scale > 0.001 && scale < 0.01 &&
                    offset > 100 && offset < 200) {
                    LOGD << "Scale/offset pattern (scale=" << scale << ", offset=" << offset
                         << ") consistent with Landsat thermal encoding";
                    return true;
                }
            } else {
                GDALClose(hDs);
            }
        }
    }

    return false;
}

// ---- Helper: read temperature data from file (R-JPEG or GeoTIFF) ----
// (TemperatureData struct is declared in thermal.h so raster_analysis.cpp can use it.)

TemperatureData readTemperatureData(const std::string &filePath) {
    TemperatureData td;

    // Try as GeoTIFF first (direct temperature or GDAL-readable)
    GDALDatasetH hDs = GDALOpen(filePath.c_str(), GA_ReadOnly);
    if (hDs) {
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
                    GDALClose(hDs);
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
                    GDALClose(hDs);
                    return td;
                }
            }
        }
        GDALClose(hDs);
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
    double mapX = geoTransform[0] + geoTransform[1] * px + geoTransform[2] * py;
    double mapY = geoTransform[3] + geoTransform[4] * px + geoTransform[5] * py;

    if (!projection.empty()) {
        OGRSpatialReferenceH hSrs = OSRNewSpatialReference(nullptr);
        OGRSpatialReferenceH hWgs84 = OSRNewSpatialReference(nullptr);
        char* wktPtr = const_cast<char*>(projection.c_str());

        if (OSRImportFromWkt(hSrs, &wktPtr) == OGRERR_NONE &&
            OSRImportFromEPSG(hWgs84, 4326) == OGRERR_NONE) {
            OGRCoordinateTransformationH hCT = OCTNewCoordinateTransformation(hSrs, hWgs84);
            if (hCT) {
                if (OCTTransform(hCT, 1, &mapX, &mapY, nullptr)) {
                    geoX = mapX;
                    geoY = mapY;
                } else {
                    geoX = mapX;
                    geoY = mapY;
                }
                OCTDestroyCoordinateTransformation(hCT);
            }
        }
        OSRDestroySpatialReference(hWgs84);
        OSRDestroySpatialReference(hSrs);
    } else {
        geoX = mapX;
        geoY = mapY;
    }
}



} // namespace ddb
