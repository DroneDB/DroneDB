/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "thermal.h"
#include "thermal_internal.h"
#include "area_statistics_internal.h"
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
#include <zlib.h>

#include <fstream>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
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

// Whether the JPEG has the FFF data of a FLIR camera with a RawData record (defined with the FFF reader below)
bool hasFlirRawData(const std::string &filePath);

thread_local ThermalReadCounters readCounters;

} // anonymous namespace

const ThermalReadCounters &thermalReadCountersForTesting() {
    return readCounters;
}

// ---- JPEG marker walk ----

namespace {

// Reads size bytes of the file where it is: whether they were all there (counted in readCounters)
bool readBytes(std::istream &file, void *to, size_t size) {
    file.read(static_cast<char *>(to), static_cast<std::streamsize>(size));
    const auto got = file.gcount();
    readCounters.fileBytes += static_cast<uint64_t>(got);
    return static_cast<size_t>(got) == size;
}

// Reads size bytes of the file at offset: whether they were all there
bool readBytesAt(std::istream &file, std::streamoff offset, void *to, size_t size) {
    file.clear();
    file.seekg(offset);
    return readBytes(file, to, size);
}

// Whether the file starts as a JPEG (SOI): an R-JPEG is one, and nothing else is read for its raw thermal data
bool startsAsJpeg(const std::string &filePath) {
    std::ifstream file(io::fromUtf8(filePath), std::ios::binary);
    uint8_t soi[2] = {};
    return file.is_open() && readBytes(file, soi, 2) && soi[0] == 0xFF && soi[1] == 0xD8;
}

// A segment of a JPEG: its marker, and where its payload (after the length) is and its size
struct JpegSegment {
    uint8_t marker = 0;
    std::streamoff offset = 0;
    size_t size = 0;
};

// Where the walk of a JPEG ended: whether the file is a JPEG, and the offset of the marker it stopped at (the scan, or
// the first marker it could not walk past: a byte that is no marker, a length below 2, a segment past the end of the
// file, the end of the image, one of the bounds); the segments before it are those that were walked
struct JpegWalk {
    bool jpeg = false;
    std::streamoff end = 0;
};

// Called for each segment with a length that is whole in the file, before the scan: false stops the walk. It may read
// the file anywhere (readBytesAt): the walk seeks to the next segment itself
using JpegVisit = std::function<bool(std::istream &file, const JpegSegment &segment)>;

// Walks the markers of a JPEG up to its scan (SOS) or its end (EOI): one walker for the probes and readers of the raw
// thermal data (the FFF data of FLIR, the APP3 segments of DJI, the FLIR segment without FFF records). The markers are
// read in sequence (fill bytes, RSTn, TEM and SOI have no length and are stepped over without a seek) and the payload
// of a segment is skipped with one seek, at most kMaxJpegMarkers markers and kMaxJpegFillBytes fill bytes in all: the
// scan of a dataset walks every DJI picture, and what a crafted file holds must not make it walk tens of millions of
// markers
JpegWalk walkJpegSegments(std::istream &file, const JpegVisit &visit) {
    JpegWalk walk;
    file.clear();
    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    uint8_t soi[2] = {};
    if (fileSize < 4 || !readBytesAt(file, 0, soi, 2) || soi[0] != 0xFF || soi[1] != 0xD8) return walk;
    walk.jpeg = true;

    std::streamoff pos = 2;
    size_t markers = 0;
    size_t fillBytes = 0;
    while (true) {
        walk.end = pos;
        uint8_t bytes[2] = {};
        if (!readBytes(file, bytes, 2) || bytes[0] != 0xFF) return walk;
        std::streamoff at = pos + 2;
        uint8_t marker = bytes[1];
        while (marker == 0xFF) { // a fill byte
            if (++fillBytes > kMaxJpegFillBytes || !readBytes(file, &marker, 1)) return walk;
            at++;
        }
        if (++markers > kMaxJpegMarkers) return walk;
        if (marker == 0xDA || marker == 0xD9) return walk; // start of scan, end of image: the segments are over
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) { // no length
            pos = at;
            continue;
        }

        if (!readBytes(file, bytes, 2)) return walk;
        const std::streamoff length = static_cast<std::streamoff>(bytes[0]) * 256 + bytes[1];
        if (length < 2 || at + length > fileSize) return walk;
        if (!visit(file, JpegSegment{marker, at + 2, static_cast<size_t>(length) - 2})) return walk;
        pos = at + length;
        file.clear();
        file.seekg(pos);
    }
}

} // anonymous namespace

bool isThermalPicture(const std::string &make, const std::string &model, const std::string &filePath) {
    // The wide and zoom pictures of a DJI thermal drone have the model of its thermal camera: only the raw thermal image
    // in the file tells the thermal picture. The FLIR cores DJI sold (Zenmuse XT, XT R, XT2) write "DJI" as the make
    // and "FLIR" or "XT2" as the model: their raw thermal image is the RawData record of their FFF data
    if (isDjiMake(make)) return hasDjiRawThermal(filePath) || hasFlirRawData(filePath);
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
                // A size that is no raw thermal image (not positive, over kMaxRawThermalPixels) is not the file's
                const int64_t width = rawWidthIt->toInt64();
                const int64_t height = rawHeightIt->toInt64();
                if (width > 0 && height > 0 && width <= std::numeric_limits<int>::max() &&
                    height <= std::numeric_limits<int>::max() &&
                    static_cast<uint64_t>(width) * static_cast<uint64_t>(height) <= kMaxRawThermalPixels) {
                    info.rawWidth = static_cast<int>(width);
                    info.rawHeight = static_cast<int>(height);
                }
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

// Search for the raw thermal data blob: a contiguous block of rawWidth * rawHeight * 2 bytes near the beginning of
// the FLIR APP1 data area (data, read from the offset dataOffset of the file)
static void locateRawData(const std::vector<uint8_t> &data, size_t dataOffset, FlirSegmentInfo &info) {
    const size_t expectedRawSize = static_cast<size_t>(info.rawWidth) * info.rawHeight * 2;

    for (size_t searchPos = 0; searchPos + expectedRawSize <= data.size(); searchPos++) {
        // Heuristic: raw thermal data typically has values in a reasonable range
        // Check first few pixels to validate
        uint16_t firstPixel = data[searchPos] | (static_cast<uint16_t>(data[searchPos + 1]) << 8);
        uint16_t midPixel = data[searchPos + expectedRawSize / 2] |
                           (static_cast<uint16_t>(data[searchPos + expectedRawSize / 2 + 1]) << 8);

        // Typical raw thermal values are in range 5000-30000 for room temp scenes
        if (firstPixel > 2000 && firstPixel < 50000 &&
            midPixel > 2000 && midPixel < 50000) {
            info.rawDataOffset = dataOffset + searchPos;
            info.rawDataSize = expectedRawSize;
            info.found = true;
            LOGD << "Found raw thermal data at offset " << info.rawDataOffset
                 << " (" << info.rawWidth << "x" << info.rawHeight << ")";
            break;
        }
    }
}

// Locates the raw UInt16 thermal block of a FLIR APP1 segment that holds no FFF records (a file with them is read
// from its RawData record, see fffRawThermal): the segment after the magic and 3 index bytes holds the raw values,
// so the block is found by its size (from the XMP, or a common sensor size) and the range of its values. The FLIR
// segment is the first APP1 segment whose payload starts with "FLIR\0" (walkJpegSegments), and the block is looked for
// in the segments from it to the scan, at most kMaxFlirDataBytes: only they are read, never the scan nor a file that is
// no JPEG (the probe of a GeoTIFF of gigabytes reads its first 2 bytes)
static FlirSegmentInfo findFlirSegment(const std::string &filePath, const FlirTags &tags) {
    FlirSegmentInfo info;

    std::ifstream file(io::fromUtf8(filePath), std::ios::binary);
    if (!file.is_open()) return info;

    // The offset just after the "FLIR\0" magic, and where the segments before the scan end
    std::optional<std::streamoff> flirSegmentStart;
    const JpegWalk walk = walkJpegSegments(file, [&](std::istream &in, const JpegSegment &segment) {
        if (!flirSegmentStart && segment.marker == 0xE1 && segment.size >= 5) {
            uint8_t magic[5] = {};
            if (readBytesAt(in, segment.offset, magic, 5) && std::memcmp(magic, "FLIR\0", 5) == 0) {
                LOGD << "Found FLIR APP1 segment at offset " << segment.offset - 4 << ", length " << segment.size + 2;
                flirSegmentStart = segment.offset + 5;
            }
        }
        return true;
    });
    if (!flirSegmentStart) {
        LOGD << "No FLIR APP1 segment found in " << filePath;
        return info;
    }

    // After FLIR magic, skip 3 bytes (segment index info), then the FFF header (64 bytes)
    const size_t dataSize = std::min(static_cast<size_t>(walk.end - *flirSegmentStart), kMaxFlirDataBytes);
    if (dataSize < 3 + 64) return info;

    findRawDimensions(tags, dataSize - 3, info);
    if (info.rawWidth == 0 || info.rawHeight == 0) return info;

    std::vector<uint8_t> data(dataSize);
    if (!readBytesAt(file, *flirSegmentStart, data.data(), dataSize)) return info;
    locateRawData(data, static_cast<size_t>(*flirSegmentStart), info);
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

} // anonymous namespace

// The FFF data of the FLIR APP1 segments of a JPEG file, in the order of their index: empty when the file is no JPEG,
// has none, or when a segment is missing. The markers are walked up to the scan (walkJpegSegments) and only the
// payloads of the FLIR APP1 segments are read (at most 256 of 64 KB): the scan of a dataset asks this of every DJI
// picture (isThermalPicture), which is not read whole. A segment of another FFF file (another last index), or an index
// seen twice, is not part of this one: the first segment of an index is kept, whatever the order of the segments
std::vector<uint8_t> readFlirFffData(const std::string &filePath) {
    std::ifstream file(io::fromUtf8(filePath), std::ios::binary);
    if (!file.is_open()) return {};

    struct Part {
        std::streamoff offset = 0;
        size_t size = 0;
        bool present = false;
    };
    std::vector<Part> parts;
    int last = -1;

    walkJpegSegments(file, [&](std::istream &in, const JpegSegment &segment) {
        if (segment.marker != 0xE1 || segment.size < 8) return true;
        // "FLIR\0", a byte, the index of the segment and the index of the last one
        uint8_t header[8] = {};
        if (!readBytesAt(in, segment.offset, header, 8) || std::memcmp(header, "FLIR\0", 5) != 0) return true;
        const int index = header[6];
        const int lastIndex = header[7];
        if (last < 0) {
            last = lastIndex;
            parts.resize(static_cast<size_t>(last) + 1);
        }
        if (lastIndex == last && index <= last && !parts[static_cast<size_t>(index)].present)
            parts[static_cast<size_t>(index)] = {segment.offset + 8, segment.size - 8, true};
        return true;
    });

    size_t total = 0;
    for (const Part &part : parts) {
        if (!part.present) return {};
        total += part.size;
    }
    std::vector<uint8_t> fff(total);
    size_t at = 0;
    for (const Part &part : parts) {
        if (!readBytesAt(file, part.offset, fff.data() + at, part.size)) return {};
        at += part.size;
    }
    return fff;
}

namespace {

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
    result.data = readFlirFffData(filePath);
    const auto records = fffRecords(result.data);
    if (!records) return std::nullopt;
    result.records = *records;
    return result;
}

bool hasFlirRawData(const std::string &filePath) {
    const auto fff = readFlirFff(filePath);
    return fff && fff->records.rawData;
}

uint32_t bigEndian32(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

} // anonymous namespace

// The PNG image of a RawData record (the format of the FLIR handheld cameras: E4 to E8, i3 to i7, the ONE, T420,
// T630sc...): the raw counts, width x height int16u. Only what FLIR writes is read: a greyscale image of 16 bits, not
// interlaced, of the size of the record, whose chunks have their CRC; anything else is nothing (no raw data). The
// samples are taken in the byte order of the record, not in the big-endian order of the PNG format: the cameras whose
// records are little-endian write them byte-swapped (as ExifTool and Thermimage read them), the old ThermaCAM ones
// (P20, S65, E45, E65, EX320), whose records are big-endian, in the order of the format. Decoded here with zlib: the
// host never opens the data of a user with GDAL (gdal_open_policy.h), and this is read on the host. The size is at
// most kMaxRawThermalPixels, and the IDAT data must be able to inflate to the rows, before they are allocated.
std::optional<std::vector<uint16_t>> decodeFlirPng(const uint8_t *png, size_t size, int width, int height,
                                                   bool bigEndian) {
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (size < 8 || std::memcmp(png, signature, 8) != 0) return std::nullopt;
    if (width <= 0 || height <= 0 || static_cast<size_t>(width) * static_cast<size_t>(height) > kMaxRawThermalPixels)
        return std::nullopt;

    const size_t rowBytes = static_cast<size_t>(width) * 2;
    std::vector<uint8_t> idat;
    bool header = false;
    bool ended = false;
    for (size_t pos = 8; pos + 12 <= size;) {
        const size_t length = bigEndian32(png + pos);
        if (length > size - pos - 12) return std::nullopt;
        const uint8_t *type = png + pos + 4;
        const uint8_t *data = png + pos + 8;
        const uint32_t crc = static_cast<uint32_t>(crc32(0L, type, static_cast<uInt>(length + 4)));
        if (crc != bigEndian32(data + length)) {
            LOGD << "The PNG raw thermal image has a chunk with a wrong CRC";
            return std::nullopt;
        }

        if (std::memcmp(type, "IHDR", 4) == 0) {
            if (header || length != 13) return std::nullopt;
            const uint32_t w = bigEndian32(data);
            const uint32_t h = bigEndian32(data + 4);
            // bit depth, colour type, compression, filter, interlace
            if (w != static_cast<uint32_t>(width) || h != static_cast<uint32_t>(height) || data[8] != 16 ||
                data[9] != 0 || data[10] != 0 || data[11] != 0 || data[12] != 0) {
                LOGD << "The PNG raw thermal image is not a 16-bit greyscale image of " << width << " x " << height
                     << " (" << w << " x " << h << ", depth " << int(data[8]) << ", colour type " << int(data[9])
                     << ", interlace " << int(data[12]) << ")";
                return std::nullopt;
            }
            header = true;
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            if (!header) return std::nullopt;
            idat.insert(idat.end(), data, data + length);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            ended = true;
            break;
        } else if (!(type[0] & 0x20)) {
            // Another critical chunk (a palette): not the image of raw counts
            return std::nullopt;
        }
        pos += length + 12;
    }
    if (!header || !ended || idat.empty()) return std::nullopt;

    // The rows, each with its filter byte: exactly height * (1 + width * 2) bytes. Deflate gives at most 1032 bytes a
    // byte (a match of 258 bytes in 2 bits): IDAT data shorter than the rows / 1032 cannot inflate to them
    const size_t rowsSize = static_cast<size_t>(height) * (rowBytes + 1);
    constexpr uint64_t kMaxDeflateRatio = 1032;
    if (static_cast<uint64_t>(idat.size()) * kMaxDeflateRatio < static_cast<uint64_t>(rowsSize)) {
        LOGD << "The PNG raw thermal image has " << idat.size() << " bytes of IDAT data, too few for its " << rowsSize
             << " bytes of rows";
        return std::nullopt;
    }
    std::vector<uint8_t> rows(rowsSize);
    readCounters.pngInflates++;
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) return std::nullopt;
    stream.next_in = idat.data();
    stream.avail_in = static_cast<uInt>(idat.size());
    stream.next_out = rows.data();
    stream.avail_out = static_cast<uInt>(rows.size());
    const int status = inflate(&stream, Z_FINISH);
    const bool whole = status == Z_STREAM_END && stream.avail_out == 0;
    inflateEnd(&stream);
    if (!whole) {
        LOGD << "The PNG raw thermal image does not inflate to its rows (zlib status " << status << ")";
        return std::nullopt;
    }

    // The filters of the rows (PNG 9.2), 2 bytes a pixel
    constexpr size_t bpp = 2;
    std::vector<uint8_t> previous(rowBytes, 0);
    std::vector<uint16_t> counts(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; y++) {
        uint8_t *row = rows.data() + static_cast<size_t>(y) * (rowBytes + 1);
        const uint8_t filter = row[0];
        uint8_t *line = row + 1;
        for (size_t i = 0; i < rowBytes; i++) {
            const int left = i >= bpp ? line[i - bpp] : 0;
            const int up = previous[i];
            const int upLeft = i >= bpp ? previous[i - bpp] : 0;
            int predictor = 0;
            switch (filter) {
            case 0: break;
            case 1: predictor = left; break;
            case 2: predictor = up; break;
            case 3: predictor = (left + up) / 2; break;
            case 4: {
                const int p = left + up - upLeft;
                const int pa = std::abs(p - left), pb = std::abs(p - up), pc = std::abs(p - upLeft);
                predictor = (pa <= pb && pa <= pc) ? left : (pb <= pc ? up : upLeft);
                break;
            }
            default: return std::nullopt;
            }
            line[i] = static_cast<uint8_t>(line[i] + predictor);
        }
        std::memcpy(previous.data(), line, rowBytes);
        uint16_t *out = counts.data() + static_cast<size_t>(y) * width;
        for (int x = 0; x < width; x++) {
            const uint8_t b0 = line[x * 2], b1 = line[x * 2 + 1];
            out[x] = bigEndian ? static_cast<uint16_t>((b0 << 8) | b1) : static_cast<uint16_t>(b0 | (b1 << 8));
        }
    }
    return counts;
}

namespace {

// The raw thermal image of the RawData record: its width (int16u at 0x02) and height (int16u at 0x04), and the raw
// counts from 0x20 on: int16u in the byte order of the record, or a PNG image (decodeFlirPng). The size is the one of
// the raw image, not of the picture: a portrait image (240 x 320, 120 x 160) stays one. A size over
// kMaxRawThermalPixels is no raw image, whatever the record holds: nothing is allocated for it.
std::optional<RawThermalData> fffRawThermal(const std::vector<uint8_t> &fff, const FffRecord &record) {
    if (record.length < 0x20) return std::nullopt;
    const int width = get16(fff, record.offset + 0x02, record.bigEndian);
    const int height = get16(fff, record.offset + 0x04, record.bigEndian);
    if (width <= 0 || height <= 0) return std::nullopt;
    const size_t pixelCount = static_cast<size_t>(width) * height;
    if (pixelCount > kMaxRawThermalPixels) return std::nullopt;

    const size_t start = record.offset + 0x20;
    const uint8_t pngSignature[] = {0x89, 'P', 'N', 'G'};
    if (record.length >= 0x24 && std::memcmp(&fff[start], pngSignature, 4) == 0) {
        auto counts = decodeFlirPng(&fff[start], record.length - 0x20, width, height, record.bigEndian);
        if (!counts) {
            LOGD << "The PNG raw thermal image of the FLIR RawData record cannot be read";
            return std::nullopt;
        }
        RawThermalData raw;
        raw.data = std::move(*counts);
        raw.width = width;
        raw.height = height;
        raw.valid = true;
        return raw;
    }

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

// The APP3 segments are found by the marker walk (walkJpegSegments): an empty one carries nothing and is skipped, and
// a file whose payloads add up to more than kMaxRawThermalPixels * 2 bytes, or are split in more than
// kMaxDjiRawThermalChunks segments, has no raw thermal image (a bad file must not make the scan allocate for it)
DjiRawThermal findDjiRawThermal(const std::string &filePath) {
    DjiRawThermal result;

    std::ifstream file(io::fromUtf8(filePath), std::ios::binary);
    if (!file.is_open()) return result;

    int pictureWidth = 0, pictureHeight = 0;
    size_t total = 0;
    bool tooLarge = false;
    std::vector<DjiRawThermal::Chunk> chunks;

    const JpegWalk walk = walkJpegSegments(file, [&](std::istream &in, const JpegSegment &segment) {
        const uint8_t marker = segment.marker;
        if (marker == 0xE3) {
            if (segment.size == 0) return true;
            total += segment.size;
            if (total > kMaxRawThermalPixels * 2 || chunks.size() >= kMaxDjiRawThermalChunks) {
                tooLarge = true;
                return false;
            }
            chunks.push_back({segment.offset, segment.size});
        } else if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            // The frame header: precision (1 byte), height and width (2 bytes each)
            uint8_t frame[5];
            if (segment.size >= 5 && readBytesAt(in, segment.offset, frame, 5)) {
                pictureHeight = frame[1] * 256 + frame[2];
                pictureWidth = frame[3] * 256 + frame[4];
            }
        }
        return true;
    });

    if (!walk.jpeg || tooLarge || chunks.empty() || total == 0) return result;
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

// The raw counts are read straight into the image (its bytes, then their little-endian values in place): one buffer
// of width * height * 2 bytes, not a second one for the bytes of the segments
RawThermalData readDjiRawThermal(const std::string &filePath) {
    RawThermalData result;
    const DjiRawThermal dji = findDjiRawThermal(filePath);
    if (!dji.found) return result;

    std::ifstream file(io::fromUtf8(filePath), std::ios::binary);
    if (!file.is_open()) return result;

    const size_t pixelCount = static_cast<size_t>(dji.width) * dji.height;
    result.data.resize(pixelCount);
    uint8_t *bytes = reinterpret_cast<uint8_t *>(result.data.data());
    size_t at = 0;
    for (const auto &chunk : dji.chunks) {
        if (!readBytesAt(file, chunk.offset, bytes + at, chunk.size)) return RawThermalData();
        at += chunk.size;
    }
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

RawThermalData extractRJpegRawThermal(const std::string &filePath) {
    // A raster that is not a JPEG has no raw thermal segments to look for: only its first 2 bytes are read
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

    size_t pixelCount = static_cast<size_t>(seg.rawWidth) * seg.rawHeight;
    result.data.resize(pixelCount);
    if (!readBytesAt(file, static_cast<std::streamoff>(seg.rawDataOffset), result.data.data(), pixelCount * 2))
        return RawThermalData();

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

        if (dt == GDT_UInt16 && pixelCount > kMaxRawThermalPixels) {
            // Not read whole: a request must not allocate gigabytes for the counts of a large raster, which are read a
            // window at a time (ThermalBandReader)
            LOGD << "The " << w << " x " << h << " raw counts of " << filePath << " are not read whole";
        } else if (dt == GDT_UInt16) {
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

        // FLIR XMP: what the camera was set to for the measurement. Not its temperatures (Xmp.FLIR.
        // ReflectedApparentTemperature, AtmosphericTemperature, IRWindowTemperature): no real file has them (the FLIR
        // XMP of the Vue Pro R, Duo Pro R and Zenmuse XT, XT R and XT2 holds none of them), so their unit (the
        // CameraInfo record has K) is not known, and a temperature off by 273.15 would move every pixel. Not read, the
        // terms of the reflection, the atmosphere and the window are left out, as for a file that does not say them
        cal.emissivity = xmpNumber({"Xmp.FLIR.Emissivity"});
        cal.objectDistance = xmpNumber({"Xmp.FLIR.ObjectDistance", "Xmp.FLIR.SubjectDistance"});
        cal.relativeHumidity = humidityFraction(xmpNumber({"Xmp.FLIR.RelativeHumidity"}));
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
        cal.reflectedApparentTemperature = xmpNumber({"Xmp.drone-dji.ThermalReflection"});
        cal.atmosphericTemperature = xmpNumber({"Xmp.drone-dji.ThermalAtmosphericTemperature"});
        LOGD << "No thermal calibration (Planck constants) in the tags of " << filePath;
    } catch (const std::exception &e) {
        LOGD << "Error reading thermal calibration: " << e.what();
    }

    return cal;
}

// ---- Planck formula ----

namespace {

// The object signal of a raw count: what the camera would count for the object alone, seen with an emissivity of 1
// through no atmosphere and no window. The equations of the FLIR measurement (as Thermimage's raw2temp and ExifTool's
// documentation of the FLIR tags give them, the formula of the FLIR software): the count is the object's signal through
// the atmosphere (transmission tau, the distance split in two halves, one each side of the window) and the IR window
// (transmission IRT), plus the emission of the atmosphere and of the window and the reflection of the surroundings on
// the object (emissivity E, reflected apparent temperature):
//   obj = raw / (E tau IRT tau) - (1 - tau) / (E tau) atm - (1 - tau) / (E tau IRT tau) atm
//         - (1 - IRT) / (E tau IRT) window - (1 - E) / E reflected
// where atm, window and reflected are the counts of a black body at the atmospheric, window and reflected temperatures
// (the inverse of the Planck formula, planckCount). A term whose parameters the file does not say is left out (as for a
// camera set to them): the emissivity without the reflected temperature, the atmosphere without the distance, the
// atmospheric temperature, the humidity or one of its five coefficients, the window without its temperature. So the
// counts of a calibration with the Planck constants alone are the object's (obj = raw). obj = raw * gain - offset.
struct ObjectSignal {
    double gain = 1.0;
    double offset = 0.0;
};

// The count of a black body at t degrees Celsius: R1 / (R2 (exp(B / T) - F)) - O
double planckCount(double celsius, const ThermalCalibration &cal) {
    return cal.planckR1 / (cal.planckR2 * (std::exp(cal.planckB / (celsius + 273.15)) - cal.planckF)) - cal.planckO;
}

ObjectSignal objectSignal(const ThermalCalibration &cal) {
    // The emissivity, with the reflected temperature it needs
    double e = 1.0;
    double reflected = 0.0;
    if (cal.emissivity && cal.reflectedApparentTemperature && *cal.emissivity > 0.0 && *cal.emissivity <= 1.0) {
        e = *cal.emissivity;
        reflected = planckCount(*cal.reflectedApparentTemperature, cal);
    }

    // The transmission of the atmosphere over half the object distance (the water vapour of the humidity at the
    // atmospheric temperature, two absorption bands mixed by X)
    double tau = 1.0;
    double atmosphere = 0.0;
    if (cal.objectDistance && cal.atmosphericTemperature && cal.relativeHumidity && cal.atmosphericTransAlpha1 &&
        cal.atmosphericTransAlpha2 && cal.atmosphericTransBeta1 && cal.atmosphericTransBeta2 && cal.atmosphericTransX &&
        *cal.objectDistance >= 0.0 && *cal.relativeHumidity >= 0.0) {
        const double t = *cal.atmosphericTemperature;
        const double h2o = *cal.relativeHumidity * std::exp(1.5587 + 0.06939 * t - 0.00027816 * t * t + 0.00000068455 * t * t * t);
        const double path = std::sqrt(*cal.objectDistance / 2.0);
        const double x = *cal.atmosphericTransX;
        const double transmission =
            x * std::exp(-path * (*cal.atmosphericTransAlpha1 + *cal.atmosphericTransBeta1 * std::sqrt(h2o))) +
            (1.0 - x) * std::exp(-path * (*cal.atmosphericTransAlpha2 + *cal.atmosphericTransBeta2 * std::sqrt(h2o)));
        if (std::isfinite(transmission) && transmission > 0.0) {
            tau = transmission;
            atmosphere = planckCount(t, cal);
        }
    }

    // The IR window, with its temperature
    double irt = 1.0;
    double window = 0.0;
    if (cal.irWindowTransmission && cal.irWindowTemperature && *cal.irWindowTransmission > 0.0 &&
        *cal.irWindowTransmission <= 1.0) {
        irt = *cal.irWindowTransmission;
        window = planckCount(*cal.irWindowTemperature, cal);
    }

    // A term that is 0 is left out, so that a black body that cannot be counted (its Planck count not finite) matters
    // only when its factor does
    const auto term = [](double factor, double count) { return factor == 0.0 ? 0.0 : factor * count; };
    ObjectSignal signal;
    signal.gain = 1.0 / (e * tau * irt * tau);
    signal.offset = term((1.0 - tau) / (e * tau), atmosphere) + term((1.0 - tau) / (e * tau * irt * tau), atmosphere) +
                    term((1.0 - irt) / (e * tau * irt), window) + term((1.0 - e) / e, reflected);
    return signal;
}

// The temperature of the object signal: T = B / ln(R1 / (R2 (obj + O)) + F) - 273.15. A signal outside the range of
// the calibration has no temperature (NaN: no value), not 0 degrees Celsius
double objectTemperature(double obj, const ThermalCalibration &cal) {
    constexpr double noTemperature = std::numeric_limits<double>::quiet_NaN();
    const double denom = cal.planckR2 * (obj + cal.planckO);
    if (!(std::abs(denom) >= 1e-30)) return noTemperature;

    const double arg = cal.planckR1 / denom + cal.planckF;
    if (!(arg > 1.0)) return noTemperature;

    const double tempK = cal.planckB / std::log(arg);
    if (!std::isfinite(tempK) || tempK <= 0.0) return noTemperature;
    return tempK - 273.15;
}

} // anonymous namespace

double rawToTemperature(uint16_t raw, const ThermalCalibration &cal) {
    const ObjectSignal signal = objectSignal(cal);
    return objectTemperature(static_cast<double>(raw) * signal.gain - signal.offset, cal);
}

namespace {

// The temperatures of count raw counts, as rawToTemperatureMap gives them (the one conversion of a grid of counts)
void rawCountsToTemperatures(const uint16_t *raw, size_t count, const ThermalCalibration &cal, float *temps) {
    const ObjectSignal signal = objectSignal(cal);
    for (size_t i = 0; i < count; i++)
        temps[i] = static_cast<float>(objectTemperature(static_cast<double>(raw[i]) * signal.gain - signal.offset, cal));
}

} // anonymous namespace

std::vector<float> rawToTemperatureMap(const RawThermalData &raw, const ThermalCalibration &cal) {
    std::vector<float> temps(raw.data.size());
    rawCountsToTemperatures(raw.data.data(), raw.data.size(), cal, temps.data());
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
    // The segments of an R-JPEG: a raster that is no JPEG (a GeoTIFF, a COG of gigabytes) has none, and only its first
    // 2 bytes are read for them
    if (startsAsJpeg(filePath)) {
        // The raw thermal image of a DJI camera, whatever its model (the probe is also what reads it)
        if (hasDjiRawThermal(filePath)) return true;
        // The raw thermal image of a FLIR camera: the RawData record of its FFF data, else the raw block of its APP1
        // segment
        if (const auto fff = readFlirFff(filePath)) {
            if (fff->records.rawData) return true;
        } else if (findFlirSegment(filePath, tags).found) {
            return true;
        }
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

// ---- The first band of a thermal raster GDAL reads, a window at a time ----

struct ThermalBandReader::Impl {
    std::string path;
    GDALDatasetUniquePtr dataset;
    GDALRasterBandH band = nullptr;
    int width = 0;
    int height = 0;
    int blockHeight = 1;
    bool hasGeoTransform = false;
    double geoTransform[6] = {};
    std::string projection;
    bool rawCounts = false;
    ThermalCalibration calibration; // of the raw counts, when valid
};

ThermalBandReader::ThermalBandReader(std::unique_ptr<Impl> state) : impl(std::move(state)) {}

ThermalBandReader::~ThermalBandReader() = default;

std::unique_ptr<ThermalBandReader> ThermalBandReader::open(const std::string &filePath) {
    auto impl = std::make_unique<Impl>();
    impl->path = filePath;
    impl->dataset = raster::openInputRaster(filePath);
    if (!impl->dataset) return nullptr;
    GDALDatasetH hDs = datasetHandle(impl->dataset);
    if (GDALGetRasterCount(hDs) < 1) return nullptr;
    impl->band = GDALGetRasterBand(hDs, 1);
    if (impl->band == nullptr) return nullptr;
    const GDALDataType dt = GDALGetRasterDataType(impl->band);
    if (dt != GDT_Float32 && dt != GDT_Float64 && dt != GDT_UInt16) return nullptr;

    impl->width = GDALGetRasterXSize(hDs);
    impl->height = GDALGetRasterYSize(hDs);
    int blockWidth = 0;
    int blockHeight = 0;
    GDALGetBlockSize(impl->band, &blockWidth, &blockHeight);
    impl->blockHeight = std::max(1, blockHeight);
    impl->hasGeoTransform = (GDALGetGeoTransform(hDs, impl->geoTransform) == CE_None);
    const char *proj = GDALGetProjectionRef(hDs);
    if (proj) impl->projection = proj;

    if (dt == GDT_UInt16) {
        // Raw values needing Planck conversion
        impl->rawCounts = true;
        impl->calibration = extractThermalCalibration(filePath);
        if (!impl->calibration.valid) {
            LOGD << "No valid thermal calibration for " << filePath << ", returning raw UInt16 values as float";
        }
    }
    return std::unique_ptr<ThermalBandReader>(new ThermalBandReader(std::move(impl)));
}

const std::string &ThermalBandReader::path() const { return impl->path; }
int ThermalBandReader::width() const { return impl->width; }
int ThermalBandReader::height() const { return impl->height; }
int ThermalBandReader::blockHeight() const { return impl->blockHeight; }
bool ThermalBandReader::hasGeoTransform() const { return impl->hasGeoTransform; }
const double *ThermalBandReader::geoTransform() const { return impl->geoTransform; }
const std::string &ThermalBandReader::projection() const { return impl->projection; }
bool ThermalBandReader::hasRawCounts() const { return impl->rawCounts; }

bool ThermalBandReader::read(int x, int y, int w, int h, std::vector<float> &temperatures,
                             std::vector<uint16_t> &rawCounts) const {
    const size_t pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
    countAreaWindow(pixels);
    temperatures.resize(pixels);
    if (!impl->rawCounts) {
        // Direct temperature values
        rawCounts.clear();
        return GDALRasterIO(impl->band, GF_Read, x, y, w, h, temperatures.data(), w, h, GDT_Float32, 0, 0) == CE_None;
    }

    rawCounts.resize(pixels);
    if (GDALRasterIO(impl->band, GF_Read, x, y, w, h, rawCounts.data(), w, h, GDT_UInt16, 0, 0) != CE_None)
        return false;
    if (impl->calibration.valid) {
        rawCountsToTemperatures(rawCounts.data(), pixels, impl->calibration, temperatures.data());
    } else {
        for (size_t p = 0; p < pixels; p++) temperatures[p] = static_cast<float>(rawCounts[p]);
    }
    return true;
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
