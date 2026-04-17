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

bool isDirectTemperatureRaster(const std::string &filePath) {
    GDALDatasetH hDs = GDALOpen(filePath.c_str(), GA_ReadOnly);
    if (!hDs) return false;

    int nBands = GDALGetRasterCount(hDs);
    if (nBands != 1) {
        GDALClose(hDs);
        return false;
    }

    GDALRasterBandH hBand = GDALGetRasterBand(hDs, 1);
    GDALDataType dt = GDALGetRasterDataType(hBand);

    // Float32 single-band rasters with typical temperature ranges
    if (dt == GDT_Float32 || dt == GDT_Float64) {
        // Check statistics to see if values are in a temperature range
        double bMin, bMax, bMean, bStdDev;
        if (GDALComputeRasterStatistics(hBand, TRUE, &bMin, &bMax, &bMean, &bStdDev, nullptr, nullptr) == CE_None) {
            // Typical thermal range: -50 to +600 °C
            if (bMin >= -100 && bMax <= 700 && bMean > -50 && bMean < 500) {
                GDALClose(hDs);
                LOGD << "Detected direct temperature raster: range " << bMin << " to " << bMax << " °C";
                return true;
            }
        }
    }

    GDALClose(hDs);
    return false;
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

    return false;
}

// ---- Helper: read temperature data from file (R-JPEG or GeoTIFF) ----

struct TemperatureData {
    std::vector<float> temperatures;
    int width = 0;
    int height = 0;
    bool hasGeoTransform = false;
    double geoTransform[6] = {};
    std::string projection;
};

static TemperatureData readTemperatureData(const std::string &filePath) {
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
                    td.temperatures = rawToTemperatureMap(raw, cal);
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
        td.temperatures = rawToTemperatureMap(raw, cal);
        td.width = raw.width;
        td.height = raw.height;
        return td;
    }

    return td;
}

// ---- Helper: convert pixel to geo coordinates ----
static void pixelToGeo(double px, double py, const double geoTransform[6],
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

// ---- JSON query functions ----

std::string getThermalInfoJson(const std::string &filePath) {
    ThermalCalibration cal = extractThermalCalibration(filePath);

    // Determine if direct temperature
    bool isDirect = isDirectTemperatureRaster(filePath);

    int width = 0, height = 0;
    float tMin = 0, tMax = 0;
    bool hasStats = false;

    // Try GDAL statistics first (efficient for large files)
    GDALDatasetH hDs = GDALOpen(filePath.c_str(), GA_ReadOnly);
    if (hDs) {
        width = GDALGetRasterXSize(hDs);
        height = GDALGetRasterYSize(hDs);
        int nBands = GDALGetRasterCount(hDs);

        if (nBands >= 1) {
            GDALRasterBandH hBand = GDALGetRasterBand(hDs, 1);
            GDALDataType dt = GDALGetRasterDataType(hBand);

            double bMin, bMax, bMean, bStdDev;
            if (GDALComputeRasterStatistics(hBand, TRUE, &bMin, &bMax, &bMean, &bStdDev, nullptr, nullptr) == CE_None) {
                if (isDirect || dt == GDT_Float32 || dt == GDT_Float64) {
                    // Direct temperature values
                    tMin = static_cast<float>(bMin);
                    tMax = static_cast<float>(bMax);
                    hasStats = true;
                } else if (dt == GDT_UInt16 && cal.valid) {
                    // Convert raw range to temperature using calibration
                    tMin = static_cast<float>(rawToTemperature(static_cast<uint16_t>(bMin), cal));
                    tMax = static_cast<float>(rawToTemperature(static_cast<uint16_t>(bMax), cal));
                    if (tMin > tMax) std::swap(tMin, tMax);
                    hasStats = true;
                } else {
                    // No calibration: report raw value range
                    tMin = static_cast<float>(bMin);
                    tMax = static_cast<float>(bMax);
                    hasStats = true;
                }
            }
        }
        GDALClose(hDs);
    }

    // Fallback: try R-JPEG for small images
    if (!hasStats) {
        TemperatureData td = readTemperatureData(filePath);
        if (!td.temperatures.empty()) {
            width = td.width;
            height = td.height;
            tMin = std::numeric_limits<float>::max();
            tMax = std::numeric_limits<float>::lowest();
            for (float t : td.temperatures) {
                if (std::isfinite(t) && t > -273.15f && t < 1000.0f) {
                    tMin = std::min(tMin, t);
                    tMax = std::max(tMax, t);
                }
            }
            hasStats = true;
        }
    }

    if (!hasStats) {
        throw AppException("Cannot read thermal data from " + filePath);
    }

    // Try sensor detection
    std::string sensorId;
    try {
        auto &spm = SensorProfileManager::instance();
        auto det = spm.detectSensor(filePath);
        if (det.detected) sensorId = det.sensorId;
    } catch (...) {}

    json result;
    result["width"] = width;
    result["height"] = height;
    result["isDirectTemperature"] = isDirect;
    result["sensorId"] = sensorId;
    result["temperatureMin"] = tMin;
    result["temperatureMax"] = tMax;

    if (cal.valid) {
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

std::string getThermalPointJson(const std::string &filePath, int x, int y) {
    // Try GDAL first (efficient: read single pixel)
    GDALDatasetH hDs = GDALOpen(filePath.c_str(), GA_ReadOnly);
    if (hDs) {
        int w = GDALGetRasterXSize(hDs);
        int h = GDALGetRasterYSize(hDs);

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

            float temp = 0;
            float rawSensorValue = 0;
            bool hasRawSensorValue = false;
            bool gotValue = false;

            if (dt == GDT_Float32 || dt == GDT_Float64) {
                float pixVal;
                if (GDALRasterIO(hBand, GF_Read, x, y, 1, 1, &pixVal, 1, 1, GDT_Float32, 0, 0) == CE_None) {
                    temp = pixVal;
                    gotValue = true;
                }
            } else if (dt == GDT_UInt16) {
                uint16_t rawVal;
                if (GDALRasterIO(hBand, GF_Read, x, y, 1, 1, &rawVal, 1, 1, GDT_UInt16, 0, 0) == CE_None) {
                    rawSensorValue = static_cast<float>(rawVal);
                    hasRawSensorValue = true;
                    ThermalCalibration cal = extractThermalCalibration(filePath);
                    if (cal.valid) {
                        temp = static_cast<float>(rawToTemperature(rawVal, cal));
                    } else {
                        temp = static_cast<float>(rawVal);
                    }
                    gotValue = true;
                }
            }

            if (gotValue) {
                json result;
                result["temperature"] = temp;
                result["x"] = x;
                result["y"] = y;
                result["rawValue"] = hasRawSensorValue ? rawSensorValue : temp;

                // Get geo coordinates if available
                double geoTransform[6];
                if (GDALGetGeoTransform(hDs, geoTransform) == CE_None) {
                    const char* proj = GDALGetProjectionRef(hDs);
                    if (proj && std::string(proj).length() > 0) {
                        double geoX = 0, geoY = 0;
                        pixelToGeo(x + 0.5, y + 0.5, geoTransform, std::string(proj), geoX, geoY);
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

    // Fallback: R-JPEG (small images)
    TemperatureData td = readTemperatureData(filePath);
    if (td.temperatures.empty()) {
        throw AppException("Cannot read thermal data from " + filePath);
    }

    if (x < 0 || x >= td.width || y < 0 || y >= td.height) {
        throw InvalidArgsException("Pixel coordinates out of bounds: (" +
            std::to_string(x) + ", " + std::to_string(y) + ") for image " +
            std::to_string(td.width) + "x" + std::to_string(td.height));
    }

    size_t idx = static_cast<size_t>(y) * td.width + x;
    float temp = td.temperatures[idx];

    json result;
    result["temperature"] = temp;
    result["x"] = x;
    result["y"] = y;

    // Get raw value if available
    RawThermalData raw = extractRawThermalData(filePath);
    if (raw.valid && idx < raw.data.size()) {
        result["rawValue"] = raw.data[idx];
    } else {
        result["rawValue"] = temp;
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

std::string getThermalAreaStatsJson(const std::string &filePath, int x0, int y0, int x1, int y1) {
    // Try GDAL first (efficient: read only ROI)
    GDALDatasetH hDs = GDALOpen(filePath.c_str(), GA_ReadOnly);
    if (hDs) {
        int w = GDALGetRasterXSize(hDs);
        int h = GDALGetRasterYSize(hDs);
        int nBands = GDALGetRasterCount(hDs);

        // Clamp to image bounds
        x0 = std::max(0, std::min(x0, w - 1));
        y0 = std::max(0, std::min(y0, h - 1));
        x1 = std::max(0, std::min(x1, w - 1));
        y1 = std::max(0, std::min(y1, h - 1));
        if (x0 > x1) std::swap(x0, x1);
        if (y0 > y1) std::swap(y0, y1);

        if (nBands >= 1) {
            GDALRasterBandH hBand = GDALGetRasterBand(hDs, 1);
            GDALDataType dt = GDALGetRasterDataType(hBand);
            ThermalCalibration cal;
            bool needsConversion = false;

            if (dt == GDT_UInt16) {
                cal = extractThermalCalibration(filePath);
                needsConversion = cal.valid;
            }

            int roiW = x1 - x0 + 1;
            int roiH = y1 - y0 + 1;
            size_t roiCount = static_cast<size_t>(roiW) * roiH;
            std::vector<float> roiData(roiCount);

            bool gotData = false;
            if (dt == GDT_Float32 || dt == GDT_Float64) {
                gotData = (GDALRasterIO(hBand, GF_Read, x0, y0, roiW, roiH,
                    roiData.data(), roiW, roiH, GDT_Float32, 0, 0) == CE_None);
            } else if (dt == GDT_UInt16) {
                std::vector<uint16_t> rawRoi(roiCount);
                if (GDALRasterIO(hBand, GF_Read, x0, y0, roiW, roiH,
                    rawRoi.data(), roiW, roiH, GDT_UInt16, 0, 0) == CE_None) {
                    for (size_t i = 0; i < roiCount; i++) {
                        roiData[i] = needsConversion
                            ? static_cast<float>(rawToTemperature(rawRoi[i], cal))
                            : static_cast<float>(rawRoi[i]);
                    }
                    gotData = true;
                }
            }

            if (gotData) {
                std::vector<float> values;
                values.reserve(roiCount);
                for (float t : roiData) {
                    if (std::isfinite(t)) values.push_back(t);
                }

                GDALClose(hDs);

                if (values.empty()) {
                    throw AppException("No valid data in the specified area");
                }

                std::sort(values.begin(), values.end());
                float tMin = values.front();
                float tMax = values.back();

                double sum = 0;
                for (float v : values) sum += v;
                double mean = sum / values.size();

                double sqSum = 0;
                for (float v : values) {
                    double diff = v - mean;
                    sqSum += diff * diff;
                }
                double stddev = std::sqrt(sqSum / values.size());

                float median;
                size_t n = values.size();
                if (n % 2 == 0) {
                    median = (values[n / 2 - 1] + values[n / 2]) / 2.0f;
                } else {
                    median = values[n / 2];
                }

                json result;
                result["min"] = tMin;
                result["max"] = tMax;
                result["mean"] = mean;
                result["stddev"] = stddev;
                result["median"] = median;
                result["pixelCount"] = static_cast<int>(values.size());
                result["bounds"] = {
                    {"x0", x0}, {"y0", y0}, {"x1", x1}, {"y1", y1}
                };
                return result.dump();
            }
        }
        GDALClose(hDs);
    }

    // Fallback: R-JPEG (small images)
    TemperatureData td = readTemperatureData(filePath);
    if (td.temperatures.empty()) {
        throw AppException("Cannot read thermal data from " + filePath);
    }

    // Clamp to image bounds
    x0 = std::max(0, std::min(x0, td.width - 1));
    y0 = std::max(0, std::min(y0, td.height - 1));
    x1 = std::max(0, std::min(x1, td.width - 1));
    y1 = std::max(0, std::min(y1, td.height - 1));

    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);

    // Collect valid temperatures in the ROI
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
        throw AppException("No valid temperature data in the specified area");
    }

    // Statistics
    std::sort(values.begin(), values.end());
    float tMin = values.front();
    float tMax = values.back();

    double sum = 0;
    for (float v : values) sum += v;
    double mean = sum / values.size();

    double sqSum = 0;
    for (float v : values) {
        double diff = v - mean;
        sqSum += diff * diff;
    }
    double stddev = std::sqrt(sqSum / values.size());

    float median;
    size_t n = values.size();
    if (n % 2 == 0) {
        median = (values[n / 2 - 1] + values[n / 2]) / 2.0f;
    } else {
        median = values[n / 2];
    }

    json result;
    result["min"] = tMin;
    result["max"] = tMax;
    result["mean"] = mean;
    result["stddev"] = stddev;
    result["median"] = median;
    result["pixelCount"] = static_cast<int>(values.size());
    result["bounds"] = {
        {"x0", x0}, {"y0", y0}, {"x1", x1}, {"y1", y1}
    };

    return result.dump();
}

} // namespace ddb
