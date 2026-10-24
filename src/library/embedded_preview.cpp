/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "embedded_preview.h"

#include <algorithm>
#include <cmath>
#include <fstream>

#include "exceptions.h"
#include "mio.h"

namespace ddb {

namespace {

constexpr uint8_t MarkerPrefix = 0xFF;
constexpr uint8_t SOI = 0xD8;
constexpr uint8_t EOI = 0xD9;
constexpr uint8_t SOS = 0xDA;
constexpr uint8_t APP2 = 0xE2;
constexpr uint16_t MpEntryTag = 0xB002;
constexpr size_t MpEntrySize = 16;

// Start-of-frame markers (C0-CF without DHT C4, JPG C8 and DAC CC) carry the image size
bool isFrameMarker(uint8_t m) {
    return m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC;
}

// Markers without a length field
bool isStandalone(uint8_t m) {
    return m == 0x01 || (m >= 0xD0 && m <= 0xD7);
}

uint16_t readU16(const uint8_t* p, bool littleEndian) {
    return littleEndian ? static_cast<uint16_t>(p[0] | (p[1] << 8))
                        : static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t readU32(const uint8_t* p, bool littleEndian) {
    return littleEndian ? static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                              (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24)
                        : (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
                              (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// An MPF index found in an APP2 segment: its entries' offsets are relative to tiffStart
struct MpfIndex {
    uint64_t tiffStart = 0;
    std::vector<uint8_t> data;  // the segment from the TIFF header on
};

// Reads the markers of the JPEG image at start (at most limit bytes) up to its first scan.
// Fills width/height from the frame header and, when mpf is given, the MPF index.
// Returns false when the bytes do not start a JPEG image or end before a frame header.
bool readJpegHeader(std::ifstream& in, uint64_t start, uint64_t limit, int& width, int& height,
                    MpfIndex* mpf) {
    const uint64_t end = start + limit;
    uint8_t soi[2];
    in.clear();
    in.seekg(static_cast<std::streamoff>(start));
    if (!in.read(reinterpret_cast<char*>(soi), 2) || soi[0] != MarkerPrefix || soi[1] != SOI)
        return false;

    uint64_t pos = start + 2;
    while (pos + 4 <= end) {
        uint8_t marker[2];
        if (!in.read(reinterpret_cast<char*>(marker), 2) || marker[0] != MarkerPrefix)
            return false;
        pos += 2;
        if (marker[1] == MarkerPrefix) {  // fill byte: the marker code follows
            in.seekg(-1, std::ios::cur);
            pos -= 1;
            continue;
        }
        if (marker[1] == SOS || marker[1] == EOI)
            break;
        if (isStandalone(marker[1]))
            continue;

        uint8_t lengthBytes[2];
        if (!in.read(reinterpret_cast<char*>(lengthBytes), 2))
            return false;
        const uint16_t length = readU16(lengthBytes, false);
        if (length < 2 || pos + length > end)
            return false;
        const uint64_t dataStart = pos + 2;
        const size_t dataSize = length - 2u;

        if (isFrameMarker(marker[1])) {
            uint8_t frame[5];
            if (dataSize < sizeof(frame) || !in.read(reinterpret_cast<char*>(frame), sizeof(frame)))
                return false;
            height = readU16(frame + 1, false);
            width = readU16(frame + 3, false);
            if (mpf == nullptr)
                return width > 0 && height > 0;
        } else if (mpf != nullptr && marker[1] == APP2 && mpf->data.empty() && dataSize > 8) {
            std::vector<uint8_t> segment(dataSize);
            if (!in.read(reinterpret_cast<char*>(segment.data()), static_cast<std::streamsize>(dataSize)))
                return false;
            if (std::equal(segment.begin(), segment.begin() + 4, "MPF\0")) {
                mpf->tiffStart = dataStart + 4;
                mpf->data.assign(segment.begin() + 4, segment.end());
            }
        }

        pos = dataStart + dataSize;
        in.clear();
        in.seekg(static_cast<std::streamoff>(pos));
    }
    return width > 0 && height > 0;
}

// (offset in the file, size) of every image of the MPF index except the first (the main image)
std::vector<std::pair<uint64_t, uint64_t>> mpfImages(const MpfIndex& mpf) {
    std::vector<std::pair<uint64_t, uint64_t>> images;
    const auto& d = mpf.data;
    if (d.size() < 8)
        return images;
    const bool le = d[0] == 'I' && d[1] == 'I';
    if (!le && !(d[0] == 'M' && d[1] == 'M'))
        return images;
    const uint32_t ifd = readU32(d.data() + 4, le);
    if (ifd + 2u > d.size())
        return images;

    const uint16_t count = readU16(d.data() + ifd, le);
    for (uint16_t i = 0; i < count; i++) {
        const size_t e = ifd + 2u + i * 12u;
        if (e + 12 > d.size())
            break;
        if (readU16(d.data() + e, le) != MpEntryTag)
            continue;
        const uint32_t bytes = readU32(d.data() + e + 4, le);
        const uint32_t at = readU32(d.data() + e + 8, le);
        if (bytes < MpEntrySize || at + static_cast<uint64_t>(bytes) > d.size())
            break;
        for (uint32_t k = 0; k + MpEntrySize <= bytes; k += MpEntrySize) {
            const uint8_t* entry = d.data() + at + k;
            const uint32_t size = readU32(entry + 4, le);
            const uint32_t offset = readU32(entry + 8, le);
            if (offset != 0 && size != 0)  // offset 0 is the main image
                images.emplace_back(mpf.tiffStart + offset, size);
        }
        break;
    }
    return images;
}

}  // namespace

JpegLayout readJpegLayout(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw FSException("Cannot read " + io::toUtf8(path));
    in.seekg(0, std::ios::end);
    const auto fileSize = static_cast<uint64_t>(in.tellg());

    JpegLayout layout;
    MpfIndex mpf;
    if (!readJpegHeader(in, 0, fileSize, layout.width, layout.height, &mpf)) {
        layout.width = layout.height = 0;
        return layout;
    }

    for (const auto& [offset, size] : mpfImages(mpf)) {
        if (offset + size > fileSize)
            continue;
        JpegPreview preview{offset, size, 0, 0};
        if (readJpegHeader(in, offset, size, preview.width, preview.height, nullptr))
            layout.previews.push_back(preview);
    }
    return layout;
}

std::optional<JpegPreview> pickJpegPreview(const JpegLayout& layout, int minLongSide) {
    if (layout.width <= 0 || layout.height <= 0)
        return std::nullopt;
    const double aspect = static_cast<double>(layout.width) / layout.height;

    std::optional<JpegPreview> best;
    for (const auto& p : layout.previews) {
        if (std::max(p.width, p.height) < minLongSide)
            continue;
        if (p.width >= layout.width || p.height >= layout.height)
            continue;
        const double ratio = (static_cast<double>(p.width) / p.height) / aspect;
        if (std::abs(ratio - 1.0) > 0.01)
            continue;
        if (!best || static_cast<int64_t>(p.width) * p.height < static_cast<int64_t>(best->width) * best->height)
            best = p;
    }
    return best;
}

std::string jpegPreviewGdalPath(const fs::path& path, const JpegPreview& preview) {
    return "/vsisubfile/" + std::to_string(preview.offset) + "_" + std::to_string(preview.size) + "," +
           io::toUtf8(path);
}

}  // namespace ddb
