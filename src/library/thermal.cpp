/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "thermal.h"
#include "stringutil.h"
#include "exif.h"
#include "exceptions.h"
#include "coordstransformer.h"
#include "gdal_metadata.h"
#include "artefact_cache.h"
#include "gdal_raii.h"
#include "logger.h"
#include "sensorprofile.h"
#include "vegetation.h"
#include "mio.h"

#include <functional>
#include <memory>
#include <optional>
#include <gdal_priv.h>
#include <cpl_conv.h>
#include <ogr_spatialref.h>

#include <fstream>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <algorithm>
#include <limits>
#include <numeric>

namespace ddb {

// ---- Thermal sensor detection by Make/Model ----

static const std::vector<std::pair<std::string, std::string>> thermalSensorPatterns = {
    {"FLIR", ""},           // Any FLIR camera
    {"DJI", "H20T"},
    {"DJI", "ZH20T"},
    {"DJI", "Zenmuse H20T"},
    {"DJI", "H20N"},
    {"DJI", "ZH20N"},
    {"DJI", "Zenmuse H20N"},
    {"DJI", "H30T"},
    {"DJI", "ZH30T"},
    {"DJI", "Zenmuse H30T"},
    {"DJI", "M30T"},
    {"DJI", "MAVIC3T"},
    {"DJI", "M3T"},        // M3T and M3TD
    {"DJI", "Mavic 3T"},
    {"DJI", "Mavic 3 Thermal"},
    {"DJI", "MAVIC2-ENTERPRISE-ADVANCED"},
    {"DJI", "M2EA"},
    // "Mavic 3 Enterprise" is not here: it names the M3E too, which has no thermal camera (the M3T is "M3T")
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

namespace {

// Whether the make of a camera is DJI
bool isDjiMake(const std::string &make) {
    std::string makeLower = make;
    str::toLower(makeLower);
    return makeLower.find("dji") != std::string::npos;
}

} // anonymous namespace

bool isThermalPicture(const std::string &make, const std::string &model, const std::string &filePath) {
    // The wide and zoom pictures of a DJI thermal drone have the model of its thermal camera: only the raw thermal image
    // in the file tells the thermal picture
    if (isDjiMake(make)) return hasDjiRawThermal(filePath);
    return isThermalImageFromExif(make, model);
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

// The tags of the file whose FLIR segment is looked for, read only if the segment is there: null
// when the file has none, or when the caller reads no tags (the raster helper)
using FlirTags = std::function<ExifParser *()>;

// The dimensions of the raw thermal image: from the FLIR XMP, else the largest of the common
// thermal camera resolutions that fits in the data after the FLIR header (remainingData bytes)
static void findRawDimensions(const FlirTags &tags, size_t remainingData, FlirSegmentInfo &info) {
    // Try to extract width/height from XMP/EXIF first. A reader that cannot work (no metadata-util)
    // is not a problem of this file and propagates.
    if (ExifParser *parser = tags()) {
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

// Locates the raw UInt16 thermal block of a FLIR APP1 segment that holds no FFF records (a file with them is read
// from its RawData record, see fffRawThermal): the segment after the magic and 3 index bytes holds the raw values,
// so the block is found by its size (from the XMP, or a common sensor size) and the range of its values.
static FlirSegmentInfo findFlirSegment(const std::string &filePath, const FlirTags &tags) {
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

    findRawDimensions(tags, fileSize - pos, info);
    if (info.rawWidth == 0 || info.rawHeight == 0) return info;

    locateRawData(data, *flirSegmentStart, info);
    return info;
}

// ---- FLIR FFF records ----
//
// The FLIR APP1 segments of an R-JPEG hold an FFF file, split in segments of at most 64 KB: each payload is "FLIR\0",
// a byte, the index of the segment and the index of the last one, then its part of the FFF data. The FFF data starts
// with a header of 64 bytes ("FFF\0" or "AFF\0", the version at 0x14, the offset of the record directory at 0x18 and
// its number of entries at 0x1c), whose byte order is the one that reads a version from 100 to 199. Each entry of the
// directory has 32 bytes: the type of the record (int16u at 0x00), its offset in the FFF data (int32u at 0x0c) and
// its length (int32u at 0x10). The records read here are RawData (type 1: the raw thermal image) and CameraInfo
// (type 0x20: the Planck constants and the measurement parameters). A record has its own byte order: its first
// int16u reads below 0x100 in it. This is the layout ExifTool documents (Image::ExifTool::FLIR, ProcessFLIR, the
// CameraInfo and RawData tables).

namespace {

// The bytes of a JPEG file; empty when the file does not start as a JPEG (SOI), so that no other raster is read whole
std::vector<uint8_t> readJpegBytes(const std::string &filePath) {
    std::ifstream file(io::fromUtf8(filePath), std::ios::binary);
    if (!file.is_open()) return {};
    uint8_t soi[2] = {};
    file.read(reinterpret_cast<char *>(soi), 2);
    if (file.gcount() != 2 || soi[0] != 0xFF || soi[1] != 0xD8) return {};
    file.seekg(0);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

uint16_t get16(const std::vector<uint8_t> &d, size_t at, bool bigEndian) {
    return bigEndian ? static_cast<uint16_t>((d[at] << 8) | d[at + 1])
                     : static_cast<uint16_t>(d[at] | (d[at + 1] << 8));
}

uint32_t get32(const std::vector<uint8_t> &d, size_t at, bool bigEndian) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        const uint32_t byte = d[at + (bigEndian ? i : 3 - i)];
        v = (v << 8) | byte;
    }
    return v;
}

float getFloat(const std::vector<uint8_t> &d, size_t at, bool bigEndian) {
    const uint32_t bits = get32(d, at, bigEndian);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// The FFF data of the FLIR APP1 segments of a JPEG (the bytes of the file), in the order of their index: empty when the
// file has none, or when a segment is missing
std::vector<uint8_t> flirFffData(const std::vector<uint8_t> &file) {
    if (file.size() < 4 || file[0] != 0xFF || file[1] != 0xD8) return {};

    struct Part {
        size_t offset = 0;
        size_t size = 0;
        bool present = false;
    };
    std::vector<Part> parts;
    int last = -1;

    size_t pos = 2;
    while (pos + 4 <= file.size() && file[pos] == 0xFF) {
        const uint8_t marker = file[pos + 1];
        if (marker == 0xFF) { // a fill byte
            pos++;
            continue;
        }
        if (marker == 0xDA || marker == 0xD9) break; // start of scan, end of image: the segments are over
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) { // no length
            pos += 2;
            continue;
        }
        const size_t length = static_cast<size_t>(file[pos + 2]) * 256 + file[pos + 3];
        if (length < 2 || pos + 2 + length > file.size()) break;
        const size_t payload = pos + 4;
        const size_t size = length - 2;

        if (marker == 0xE1 && size >= 8 && std::memcmp(&file[payload], "FLIR\0", 5) == 0) {
            const int index = file[payload + 6];
            const int lastIndex = file[payload + 7];
            if (last < 0) {
                last = lastIndex;
                parts.resize(static_cast<size_t>(last) + 1);
            }
            // A segment of another FFF file (another last index), or one seen twice, is not part of this one
            if (lastIndex == last && index <= last && !parts[index].present)
                parts[index] = {payload + 8, size - 8, true};
        }
        pos += 2 + length;
    }

    std::vector<uint8_t> fff;
    for (const Part &part : parts) {
        if (!part.present) return {};
        fff.insert(fff.end(), file.begin() + static_cast<std::ptrdiff_t>(part.offset),
                   file.begin() + static_cast<std::ptrdiff_t>(part.offset + part.size));
    }
    return fff;
}

// A record of the FFF data: where it is, its length and its byte order
struct FffRecord {
    size_t offset = 0;
    size_t length = 0;
    bool bigEndian = true;
};

// The records of the FFF data that are read here
struct FffRecords {
    std::optional<FffRecord> rawData;
    std::optional<FffRecord> cameraInfo;
};

constexpr uint16_t kFffRawData = 0x01;
constexpr uint16_t kFffCameraInfo = 0x20;

// The RawData and CameraInfo records of the FFF data (the first of each type); nothing when it is no FFF data
std::optional<FffRecords> fffRecords(const std::vector<uint8_t> &fff) {
    if (fff.size() < 0x40) return std::nullopt;
    if (std::memcmp(fff.data(), "FFF\0", 4) != 0 && std::memcmp(fff.data(), "AFF\0", 4) != 0) return std::nullopt;

    // The byte order of the header: the one that reads a version from 100 to 199
    const auto knownVersion = [&](bool bigEndian) {
        const uint32_t version = get32(fff, 0x14, bigEndian);
        return version >= 100 && version < 200;
    };
    bool bigEndian = true;
    if (!knownVersion(true)) {
        if (!knownVersion(false)) return std::nullopt;
        bigEndian = false;
    }

    const size_t directory = get32(fff, 0x18, bigEndian);
    const size_t entries = get32(fff, 0x1c, bigEndian);
    if (directory > fff.size() || entries > (fff.size() - directory) / 32) return std::nullopt;

    FffRecords records;
    for (size_t i = 0; i < entries; i++) {
        const size_t entry = directory + i * 32;
        const uint16_t type = get16(fff, entry, bigEndian);
        if (type != kFffRawData && type != kFffCameraInfo) continue;
        auto &slot = type == kFffRawData ? records.rawData : records.cameraInfo;
        if (slot) continue;

        const size_t offset = get32(fff, entry + 0x0c, bigEndian);
        const size_t length = get32(fff, entry + 0x10, bigEndian);
        // A record outside the data, or too short to say its byte order, is not read
        if (offset > fff.size() || length > fff.size() - offset || length < 2) continue;
        // The first int16u of a record reads below 0x100 in the byte order of the record
        const bool recordBigEndian = get16(fff, offset, bigEndian) >= 0x100 ? !bigEndian : bigEndian;
        slot = FffRecord{offset, length, recordBigEndian};
    }
    return records;
}

// The FFF data of the FLIR APP1 segments of a file and its records; nothing when the file has none
struct FlirFff {
    std::vector<uint8_t> data;
    FffRecords records;
};

std::optional<FlirFff> readFlirFff(const std::string &filePath) {
    FlirFff result;
    result.data = flirFffData(readJpegBytes(filePath));
    const auto records = fffRecords(result.data);
    if (!records) return std::nullopt;
    result.records = *records;
    return result;
}

// The raw thermal image of the RawData record: its width (int16u at 0x02) and height (int16u at 0x04), and the raw
// counts from 0x20 on, int16u in the byte order of the record. Nothing when the counts are in a PNG image (the format
// of the FLIR handheld cameras): decoding it needs GDAL, which only the raster helper opens user data with, so its
// raw counts are not read (the picture is its colour preview).
std::optional<RawThermalData> fffRawThermal(const std::vector<uint8_t> &fff, const FffRecord &record) {
    if (record.length < 0x20) return std::nullopt;
    const int width = get16(fff, record.offset + 0x02, record.bigEndian);
    const int height = get16(fff, record.offset + 0x04, record.bigEndian);
    if (width <= 0 || height <= 0) return std::nullopt;

    const size_t start = record.offset + 0x20;
    const uint8_t pngSignature[] = {0x89, 'P', 'N', 'G'};
    if (record.length >= 0x24 && std::memcmp(&fff[start], pngSignature, 4) == 0) {
        LOGD << "The FLIR raw thermal image is a PNG image, which is not decoded here";
        return std::nullopt;
    }

    const size_t pixelCount = static_cast<size_t>(width) * height;
    if (record.length - 0x20 < pixelCount * 2) return std::nullopt;

    RawThermalData raw;
    raw.data.resize(pixelCount);
    for (size_t i = 0; i < pixelCount; i++) raw.data[i] = get16(fff, start + i * 2, record.bigEndian);
    raw.width = width;
    raw.height = height;
    raw.valid = true;
    return raw;
}

// The relative humidity as a fraction: a camera writes it as a fraction or as a percentage, which is over 2 (as ExifTool
// reads it). The same rule for the CameraInfo record and the FLIR XMP, so that one camera says one humidity
static std::optional<double> humidityFraction(std::optional<double> humidity) {
    if (humidity && *humidity > 2) *humidity /= 100.0;
    return humidity;
}

// The calibration of the CameraInfo record: the Planck constants (PlanckR1, PlanckB and PlanckF, floats at 0x58,
// 0x5c and 0x60; PlanckO, int32s at 0x308; PlanckR2, float at 0x30c), valid only when the five are there and are
// those of a camera (R1, B and R2 positive), and the measurement parameters: Emissivity (0x20), ObjectDistance (0x24,
// m), ReflectedApparentTemperature, AtmosphericTemperature and IRWindowTemperature (0x28, 0x2c, 0x30: K, given in
// degrees Celsius), IRWindowTransmission (0x34), RelativeHumidity (0x3c: a fraction, or a percentage when over 2, as
// ExifTool reads it, given as a fraction) and AtmosphericTransAlpha1, Alpha2, Beta1, Beta2 and X (0x70 to 0x80). A field
// beyond the end of the record, or that is not a number, is not set.
ThermalCalibration fffCameraInfoCalibration(const std::vector<uint8_t> &fff, const FffRecord &record) {
    ThermalCalibration cal;
    const auto number = [&](size_t at) -> std::optional<double> {
        if (at + 4 > record.length) return std::nullopt;
        const double value = getFloat(fff, record.offset + at, record.bigEndian);
        if (!std::isfinite(value)) return std::nullopt;
        return value;
    };
    const auto celsius = [&](size_t at) -> std::optional<double> {
        const auto kelvin = number(at);
        if (!kelvin) return std::nullopt;
        return *kelvin - 273.15;
    };

    cal.emissivity = number(0x20);
    cal.objectDistance = number(0x24);
    cal.reflectedApparentTemperature = celsius(0x28);
    cal.atmosphericTemperature = celsius(0x2c);
    cal.irWindowTemperature = celsius(0x30);
    cal.irWindowTransmission = number(0x34);
    cal.relativeHumidity = humidityFraction(number(0x3c));
    cal.atmosphericTransAlpha1 = number(0x70);
    cal.atmosphericTransAlpha2 = number(0x74);
    cal.atmosphericTransBeta1 = number(0x78);
    cal.atmosphericTransBeta2 = number(0x7c);
    cal.atmosphericTransX = number(0x80);

    const auto r1 = number(0x58);
    const auto b = number(0x5c);
    const auto f = number(0x60);
    const auto r2 = number(0x30c);
    if (r1 && b && f && r2 && 0x308 + 4 <= record.length && *r1 > 0 && *b > 0 && *r2 > 0) {
        cal.planckR1 = *r1;
        cal.planckB = *b;
        cal.planckF = *f;
        cal.planckO = static_cast<int32_t>(get32(fff, record.offset + 0x308, record.bigEndian));
        cal.planckR2 = *r2;
        cal.valid = true;
    }
    return cal;
}

} // anonymous namespace

// ---- DJI raw thermal probe ----

namespace {

// The APP3 segments of a DJI R-JPEG that hold its raw thermal image: where each one's payload is, and the size of
// the image. The rule is that of the GDAL JPEG driver (JPGDatasetCommon::ReadDJIMetadata): the payloads of the APP3
// segments before the scan add up to width * height * 2 bytes of the picture, or to 640 * 512 * 2 (the nominal sensor,
// for the models whose picture is larger than their thermal image, such as the M4T), little-endian UInt16.
struct DjiRawThermal {
    struct Chunk {
        std::streamoff offset = 0;
        size_t size = 0;
    };
    std::vector<Chunk> chunks;
    int width = 0;
    int height = 0;
    bool found = false;
};

DjiRawThermal findDjiRawThermal(const std::string &filePath) {
    DjiRawThermal result;

    std::ifstream file(io::fromUtf8(filePath), std::ios::binary);
    if (!file.is_open()) return result;

    uint8_t header[4];
    file.read(reinterpret_cast<char *>(header), 2);
    if (file.gcount() != 2 || header[0] != 0xFF || header[1] != 0xD8) return result;

    int pictureWidth = 0, pictureHeight = 0;
    size_t total = 0;
    std::vector<DjiRawThermal::Chunk> chunks;

    std::streamoff pos = 2;
    while (true) {
        file.clear();
        file.seekg(pos);
        file.read(reinterpret_cast<char *>(header), 4);
        if (file.gcount() != 4 || header[0] != 0xFF) break;

        const uint8_t marker = header[1];
        if (marker == 0xDA) break; // start of scan: the segments are over
        const int length = header[2] * 256 + header[3];
        if (length < 2) break;
        const size_t payload = static_cast<size_t>(length) - 2;

        if (marker == 0xE3) {
            total += payload;
            // A picture of a sensor of 10000 x 10000 pixels is no thermal image (a bad EXIF must not allocate 200 MB)
            if (total > static_cast<size_t>(10000) * 10000 * 2) return result;
            chunks.push_back({pos + 4, payload});
        } else if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            // The frame header: precision (1 byte), height and width (2 bytes each)
            uint8_t frame[5];
            file.read(reinterpret_cast<char *>(frame), 5);
            if (file.gcount() == 5) {
                pictureHeight = frame[1] * 256 + frame[2];
                pictureWidth = frame[3] * 256 + frame[4];
            }
        }
        pos += 2 + length;
    }

    if (chunks.empty() || total == 0) return result;
    if (pictureWidth > 0 && pictureHeight > 0 && total == static_cast<size_t>(pictureWidth) * pictureHeight * 2) {
        result.width = pictureWidth;
        result.height = pictureHeight;
    } else if (total == static_cast<size_t>(640) * 512 * 2) {
        result.width = 640;
        result.height = 512;
    } else {
        return result;
    }
    result.chunks = std::move(chunks);
    result.found = true;
    return result;
}

RawThermalData readDjiRawThermal(const std::string &filePath) {
    RawThermalData result;
    const DjiRawThermal dji = findDjiRawThermal(filePath);
    if (!dji.found) return result;

    std::ifstream file(io::fromUtf8(filePath), std::ios::binary);
    if (!file.is_open()) return result;

    std::vector<uint8_t> bytes;
    bytes.reserve(static_cast<size_t>(dji.width) * dji.height * 2);
    for (const auto &chunk : dji.chunks) {
        const size_t at = bytes.size();
        bytes.resize(at + chunk.size);
        file.seekg(chunk.offset);
        file.read(reinterpret_cast<char *>(bytes.data() + at), static_cast<std::streamsize>(chunk.size));
        if (static_cast<size_t>(file.gcount()) != chunk.size) return RawThermalData();
    }

    const size_t pixelCount = static_cast<size_t>(dji.width) * dji.height;
    result.data.resize(pixelCount);
    for (size_t i = 0; i < pixelCount; i++)
        result.data[i] = static_cast<uint16_t>(bytes[i * 2] | (static_cast<uint16_t>(bytes[i * 2 + 1]) << 8));
    result.width = dji.width;
    result.height = dji.height;
    result.valid = true;
    LOGD << "Extracted " << pixelCount << " raw thermal pixels from the DJI segments of " << filePath;
    return result;
}

} // anonymous namespace

bool hasDjiRawThermal(const std::string &filePath) {
    return findDjiRawThermal(filePath).found;
}

// ---- R-JPEG raw data extraction ----

namespace {

// Whether the file starts as a JPEG (SOI): an R-JPEG is one, and nothing else is read for its raw thermal data
bool startsAsJpeg(const std::string &filePath) {
    std::ifstream file(io::fromUtf8(filePath), std::ios::binary);
    uint8_t soi[2] = {};
    file.read(reinterpret_cast<char *>(soi), 2);
    return file.gcount() == 2 && soi[0] == 0xFF && soi[1] == 0xD8;
}

} // anonymous namespace

RawThermalData extractRJpegRawThermal(const std::string &filePath) {
    // The FLIR search reads the whole file: a raster that is not a JPEG has no raw thermal segments to look for
    if (!startsAsJpeg(filePath)) return RawThermalData();

    // DJI: the APP3 segments (the probe of FR-RAS-13: what detects the file is what reads it)
    RawThermalData dji = readDjiRawThermal(filePath);
    if (dji.valid) return dji;

    // FLIR: the RawData record of the FFF data of the APP1 segments. A file with FFF records is read only from them: a
    // raw image that is not read there (a PNG image, a record that is not whole) is not looked for elsewhere
    if (const auto fff = readFlirFff(filePath)) {
        if (!fff->records.rawData) return RawThermalData();
        auto raw = fffRawThermal(fff->data, *fff->records.rawData);
        if (!raw) return RawThermalData();
        LOGD << "Extracted " << raw->data.size() << " raw thermal pixels from the FLIR RawData record of " << filePath;
        return std::move(*raw);
    }

    // FLIR without FFF records: the raw block of the APP1 segment, found by its size and its values
    RawThermalData result;
    std::unique_ptr<ExifParser> tags;
    FlirSegmentInfo seg = findFlirSegment(filePath, [&] {
        tags = readExifParser(io::fromUtf8(filePath));
        return tags.get();
    });
    if (!seg.found) return result;

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

RawThermalData extractRawThermalData(const std::string &filePath) {
    // An R-JPEG (FLIR or DJI)
    RawThermalData rjpeg = extractRJpegRawThermal(filePath);
    if (rjpeg.valid) return rjpeg;

    // GDAL fallback: a thermal GeoTIFF with the raw UInt16 counts of the sensor. The 8-bit colour bands of an
    // R-JPEG are its preview, not raw data: no temperature comes from them (FR-RAS-14)
    LOGD << "No R-JPEG raw thermal data found, trying GDAL for " << filePath;
    RawThermalData result;

    const auto dataset = raster::openInputRaster(filePath);
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
        }
    }

    return result;
}

// ---- Calibration extraction ----

ThermalCalibration extractThermalCalibration(const std::string &filePath) {
    // FLIR: the CameraInfo record of the FFF data, where the camera writes its Planck constants. It is read from the
    // bytes of the file, without the tag reader (the raster helper has none)
    if (const auto fff = readFlirFff(filePath); fff && fff->records.cameraInfo) {
        const ThermalCalibration fromRecord = fffCameraInfoCalibration(fff->data, *fff->records.cameraInfo);
        if (fromRecord.valid) {
            LOGD << "Found the FLIR calibration in the CameraInfo record of " << filePath;
            return fromRecord;
        }
    }

    // Else the tags of the file
    ThermalCalibration cal;

    // A reader that cannot work (no metadata-util) is not a problem of this file and propagates
    const auto parserPtr = readExifParser(io::fromUtf8(filePath));
    if (!parserPtr) return cal;
    ExifParser &parser = *parserPtr;

    try {
        // The number of the first of the XMP keys the file has, when it is a number; nothing otherwise (never a default)
        const auto xmpNumber = [&](const std::initializer_list<std::string> &keys) -> std::optional<double> {
            auto it = parser.findXmpKey(keys);
            if (!it) return std::nullopt;
            try {
                const double value = std::stod(it->toString());
                if (std::isfinite(value)) return value;
            } catch (const std::invalid_argument &) {
                // Not a number: the file does not say it
            } catch (const std::out_of_range &) {
                // Out of the range of a double: the file does not say it
            }
            return std::nullopt;
        };

        // FLIR XMP: what the camera was set to for the measurement
        cal.emissivity = xmpNumber({"Xmp.FLIR.Emissivity"});
        cal.objectDistance = xmpNumber({"Xmp.FLIR.ObjectDistance", "Xmp.FLIR.SubjectDistance"});
        cal.reflectedApparentTemperature = xmpNumber({"Xmp.FLIR.ReflectedApparentTemperature"});
        cal.atmosphericTemperature = xmpNumber({"Xmp.FLIR.AtmosphericTemperature"});
        cal.relativeHumidity = humidityFraction(xmpNumber({"Xmp.FLIR.RelativeHumidity"}));
        cal.irWindowTemperature = xmpNumber({"Xmp.FLIR.IRWindowTemperature"});
        cal.irWindowTransmission = xmpNumber({"Xmp.FLIR.IRWindowTransmission"});

        // FLIR XMP: the Planck constants. The calibration is valid only with the five of them: the defaults of no
        // camera are the calibration of this one
        const auto r1 = xmpNumber({"Xmp.FLIR.PlanckR1"});
        const auto b = xmpNumber({"Xmp.FLIR.PlanckB"});
        const auto f = xmpNumber({"Xmp.FLIR.PlanckF"});
        const auto o = xmpNumber({"Xmp.FLIR.PlanckO"});
        const auto r2 = xmpNumber({"Xmp.FLIR.PlanckR2"});
        if (r1 && b && f && o && r2) {
            cal.planckR1 = *r1;
            cal.planckB = *b;
            cal.planckF = *f;
            cal.planckO = *o;
            cal.planckR2 = *r2;
            cal.valid = true;
            LOGD << "Found FLIR XMP calibration data";
            return cal;
        }

        // DJI XMP: the measurement parameters the file says, never a calibration. The raw counts of a DJI camera are not
        // those of the Planck formula: only DJI's conversion turns them into temperatures (djiRawToTemperatureMap)
        if (!cal.emissivity) cal.emissivity = xmpNumber({"Xmp.drone-dji.ThermalObjectEmissivity"});
        if (!cal.objectDistance) cal.objectDistance = xmpNumber({"Xmp.drone-dji.ThermalObjectDistance"});
        if (!cal.reflectedApparentTemperature)
            cal.reflectedApparentTemperature = xmpNumber({"Xmp.drone-dji.ThermalReflection"});
        if (!cal.atmosphericTemperature)
            cal.atmosphericTemperature = xmpNumber({"Xmp.drone-dji.ThermalAtmosphericTemperature"});
        LOGD << "No thermal calibration (Planck constants) in the tags of " << filePath;
    } catch (const std::exception &e) {
        LOGD << "Error reading thermal calibration: " << e.what();
    }

    return cal;
}

// ---- Planck formula ----

double rawToTemperature(uint16_t raw, const ThermalCalibration &cal) {
    // T = B / ln(R1 / (R2 * (raw + O)) + F) - 273.15
    // A raw count outside the range of the calibration has no temperature (NaN: no value), not 0 degrees Celsius
    constexpr double noTemperature = std::numeric_limits<double>::quiet_NaN();
    const double rawD = static_cast<double>(raw);
    const double denom = cal.planckR2 * (rawD + cal.planckO);

    if (!(std::abs(denom) >= 1e-30)) return noTemperature;

    const double arg = cal.planckR1 / denom + cal.planckF;
    if (!(arg > 1.0)) return noTemperature;

    const double tempK = cal.planckB / std::log(arg);
    if (!std::isfinite(tempK) || tempK <= 0.0) return noTemperature;
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

    const auto dataset = raster::openInputRaster(filePath);
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
bool tagsSayThermal(ExifParser &parser) {
    try {
        std::string make = parser.extractMake();
        std::string model = parser.extractModel();

        // A DJI model is not enough: the wide and zoom pictures of a thermal drone have the model of the thermal camera.
        // The raw thermal image of the file (contentSaysThermal) tells a DJI thermal picture (FR-RAS-13)
        if (isThermalImageFromExif(make, model) && !isDjiMake(make)) return true;

        // Method 2: Check for FLIR XMP namespace
        auto flirIt = parser.findXmpKey({"Xmp.FLIR.PlanckR1",
                                         "Xmp.FLIR.Emissivity",
                                         "Xmp.FLIR.ThermalMeasureMode"});
        if (flirIt) return true;

        // Method 3: Check for DJI thermal tags
        auto djiIt = parser.findXmpKey({"Xmp.drone-dji.ThermalMeasureMode",
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
    // The metadata of the dataset and of its only band, with its description
    gdalmeta::CollectOptions options;
    options.bandCount = 1;
    options.bandDescriptions = true;
    const std::vector<std::string> metaStrings = gdalmeta::collectMetadata(hDs, options);

    GDALRasterBandH hBand = GDALGetRasterBand(hDs, 1);

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

// Keywords to look for in metadata: short ones (dem, bt, lst...) are acronyms that occur inside
// ordinary words ("academic", "subtitles"), so they count only as whole words; long ones are
// specific enough to match anywhere
struct KeywordSet {
    std::vector<std::string> tokens;
    std::vector<std::string> substrings;
};

// The first keyword of the set found in the lowercase text
std::optional<std::string> findKeyword(const std::string &text, const KeywordSet &keywords) {
    for (const auto &kw : keywords.tokens) {
        if (gdalmeta::containsToken(text, kw)) return kw;
    }
    for (const auto &kw : keywords.substrings) {
        if (text.find(kw) != std::string::npos) return kw;
    }
    return std::nullopt;
}

// Whether the signals describe a thermal raster: a DEM's keywords and units rule it out, then
// thermal keywords, units and the Landsat scale/offset pattern make it one
bool looksThermal(const ThermalSignals &signals) {
    const std::string &allMeta = signals.allMeta;
    const std::string &unitStr = signals.unitStr;
    const std::string &unitLower = signals.unitLower;

    // DEM negative signals - reject early
    static const KeywordSet demKeywords = {{"dem", "dtm", "dsm"}, {"elevation", "height"}};
    static const std::vector<std::string> demUnits = {
        "m", "ft", "meter", "meters", "feet", "foot"
    };
    static const std::vector<std::string> demCrsKeywords = {
        "vertical", "geoid", "ellipsoid height"
    };

    if (const auto kw = findKeyword(allMeta, demKeywords)) {
        LOGD << "GDAL metadata contains DEM keyword '" << *kw << "', not thermal";
        return false;
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
    static const KeywordSet thermalKeywords = {
        {"lst", "tir", "bt", "lwir"},
        {"thermal", "temperature", "surface_temperature", "brightness_temperature"}};
    static const std::vector<std::string> thermalUnits = {
        "kelvin", "celsius", "degc", "degk",
        "\xc2\xb0""c", "\xc2\xb0""k"  // UTF-8 for °C, °K
    };

    if (const auto kw = findKeyword(allMeta, thermalKeywords)) {
        LOGD << "GDAL metadata contains thermal keyword '" << *kw << "'";
        return true;
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
    auto dataset = raster::openInputRaster(filePath);
    if (!dataset) return false;
    GDALDatasetH hDs = datasetHandle(dataset);
    if (GDALGetRasterCount(hDs) != 1) return false;

    const ThermalSignals signals = readThermalSignals(hDs);
    dataset.reset();
    return looksThermal(signals);
}

// Methods 4-5: what the file holds besides its tags, the DJI or FLIR segments of an R-JPEG and the GDAL
// metadata of a single-band raster
bool contentSaysThermal(const std::string &filePath, const FlirTags &tags) {
    // The raw thermal image of a DJI camera, whatever its model (the probe is also what reads it)
    if (hasDjiRawThermal(filePath)) return true;
    // The raw thermal image of a FLIR camera: the RawData record of its FFF data, else the raw block of its APP1 segment
    if (const auto fff = readFlirFff(filePath)) {
        if (fff->records.rawData) return true;
    } else if (findFlirSegment(filePath, tags).found) {
        return true;
    }
    return isThermalByGdalMetadata(filePath);
}

} // anonymous namespace

bool hasThermalTags(const std::string &filePath) {
    // A reader that cannot work (no metadata-util) is not a problem of this file and propagates
    const auto parser = readExifParser(io::fromUtf8(filePath));
    return parser && tagsSayThermal(*parser);
}

bool isThermalImage(const std::string &filePath, std::optional<bool> thermalTags) {
    if (thermalTags) {
        // The tags were read by the host: the segment is found by the common sensor sizes
        return *thermalTags || contentSaysThermal(filePath, [] { return static_cast<ExifParser *>(nullptr); });
    }

    // A reader that cannot work (no metadata-util) is not a problem of this file and propagates
    const auto parser = readExifParser(io::fromUtf8(filePath));
    if (parser && tagsSayThermal(*parser)) return true;
    return contentSaysThermal(filePath, [&parser] { return parser.get(); });
}

// ---- Helper: read temperature data from file (R-JPEG or GeoTIFF) ----
// (TemperatureData struct is declared in thermal.h so raster_analysis.cpp can use it.)

TemperatureData readTemperatureData(const std::string &filePath) {
    TemperatureData td;

    // Try as GeoTIFF first (direct temperature or GDAL-readable)
    if (const auto dataset = raster::openInputRaster(filePath)) {
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

    // Not a raster of temperatures (an 8-bit picture, a band that could not be read): nothing was read
    td.temperatures.clear();

    // Try R-JPEG extraction
    if (const auto rjpeg = readRJpegThermal(filePath)) {
        if (rjpeg->grid.temperatures.empty()) {
            // The raw counts of the camera have no calibration: they are not temperatures (FR-RAS-14)
            LOGD << "No valid thermal calibration for " << filePath << ", the R-JPEG has no temperatures";
            td.width = rjpeg->raw.width;
            td.height = rjpeg->raw.height;
        } else {
            td = rjpeg->grid;
        }
        return td;
    }

    // Raw UInt16 data of a raster that GDAL could not read as one band (nothing else holds raw data)
    RawThermalData raw = extractRawThermalData(filePath);
    if (raw.valid) {
        ThermalCalibration cal = extractThermalCalibration(filePath);
        if (cal.valid) {
            td.temperatures = rawToTemperatureMap(raw, cal);
        } else {
            LOGD << "No valid thermal calibration for " << filePath
                 << ", returning raw values as float";
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

std::optional<std::vector<float>> djiRawToTemperatureMap(const RawThermalData &raw, const std::string &filePath) {
    // DJI's conversion (dji_irp) is not available: no temperature is told from the raw counts of a DJI camera
    LOGD << "No conversion of the " << raw.data.size() << " DJI raw thermal counts of " << filePath
         << " into temperatures: it needs DJI's own conversion, which is not available";
    return std::nullopt;
}

std::optional<RJpegThermal> readRJpegThermal(const std::string &filePath) {
    RJpegThermal result;
    result.raw = extractRJpegRawThermal(filePath);
    if (!result.raw.valid) return std::nullopt;

    result.grid.width = result.raw.width;
    result.grid.height = result.raw.height;

    if (hasDjiRawThermal(filePath)) {
        // DJI: DJI's own conversion, not the Planck formula of FLIR, and no calibration (no tag is read: the raster
        // helper reads none)
        result.dji = true;
        if (auto temperatures = djiRawToTemperatureMap(result.raw, filePath))
            result.grid.temperatures = std::move(*temperatures);
        return result;
    }

    // FLIR: the calibration of the tags of the file
    result.calibration = extractThermalCalibration(filePath);
    if (result.calibration.valid)
        result.grid.temperatures = rawToTemperatureMap(result.raw, result.calibration);
    return result;
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



// ---- Temperature units (D-022)

std::optional<TemperatureUnit> parseTemperatureUnit(const std::string &unit) {
    std::string u;
    for (const char c : unit)
        if (std::isalnum(static_cast<unsigned char>(c)))
            u += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (u == "k" || u == "kelvin" || u == "degk") return TemperatureUnit::Kelvin;
    if (u == "c" || u == "celsius" || u == "degc" || u == "centigrade") return TemperatureUnit::Celsius;
    if (u == "f" || u == "fahrenheit" || u == "degf") return TemperatureUnit::Fahrenheit;
    return std::nullopt;
}

TemperatureUnit bandTemperatureUnit(const std::string &bandUnit) {
    return parseTemperatureUnit(bandUnit).value_or(TemperatureUnit::Celsius);
}

const char *temperatureUnitSymbol(TemperatureUnit unit) {
    switch (unit) {
    case TemperatureUnit::Kelvin: return "K";
    case TemperatureUnit::Fahrenheit: return "\xC2\xB0" "F";
    case TemperatureUnit::Celsius: break;
    }
    return "\xC2\xB0" "C";
}

std::optional<TemperatureUnit> thermalFormulaUnit(const std::string &formulaId) {
    if (formulaId == "CELSIUS") return TemperatureUnit::Celsius;
    if (formulaId == "KELVIN") return TemperatureUnit::Kelvin;
    if (formulaId == "FAHRENHEIT") return TemperatureUnit::Fahrenheit;
    return std::nullopt;
}

namespace {

// The unit as degrees Celsius: celsius = value * scale + offset
TemperatureConversion toCelsius(TemperatureUnit unit) {
    switch (unit) {
    case TemperatureUnit::Kelvin: return {1.0, -273.15};
    case TemperatureUnit::Fahrenheit: return {5.0 / 9.0, -32.0 * 5.0 / 9.0};
    case TemperatureUnit::Celsius: break;
    }
    return {};
}

// Degrees Celsius as the unit
TemperatureConversion fromCelsius(TemperatureUnit unit) {
    switch (unit) {
    case TemperatureUnit::Kelvin: return {1.0, 273.15};
    case TemperatureUnit::Fahrenheit: return {1.8, 32.0};
    case TemperatureUnit::Celsius: break;
    }
    return {};
}

} // anonymous namespace

TemperatureConversion temperatureConversion(TemperatureUnit from, TemperatureUnit to) {
    if (from == to) return {};
    const TemperatureConversion a = toCelsius(from);
    const TemperatureConversion b = fromCelsius(to);
    return {a.scale * b.scale, a.offset * b.scale + b.offset};
}

namespace {

ThermalFormulaConversion thermalFormulaConversionFrom(TemperatureUnit from, const std::string &formulaId) {
    const auto to = thermalFormulaUnit(formulaId);
    if (!to) {
        if (VegetationEngine::instance().getFormula(formulaId) == nullptr)
            throw RefusedArgumentException("Unknown formula: " + formulaId);
        throw RefusedArgumentException(
            "The values of a point, an area, a profile or contours take a thermal formula (CELSIUS, KELVIN, FAHRENHEIT), not " +
            formulaId);
    }
    return {temperatureConversion(from, *to), temperatureUnitSymbol(*to)};
}

} // anonymous namespace

ThermalFormulaConversion thermalFormulaConversion(const std::string &bandUnit, const std::string &formulaId) {
    return thermalFormulaConversionFrom(bandTemperatureUnit(bandUnit), formulaId);
}

ThermalFormulaConversion celsiusFormulaConversion(const std::string &formulaId) {
    return thermalFormulaConversionFrom(TemperatureUnit::Celsius, formulaId);
}

} // namespace ddb
