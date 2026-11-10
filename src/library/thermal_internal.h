/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef THERMAL_INTERNAL_H
#define THERMAL_INTERNAL_H

// Private to the R-JPEG reader (thermal.cpp), the thermal grids of the raster API (raster_analysis.cpp) and their unit
// tests: not installed, not part of the API.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ddb {

// The bounds of the walk of the markers of a JPEG up to its scan (the probes of the scan, the FFF data of FLIR, the APP3
// segments of DJI), whatever the size of the file: what lies after them is not read, as if the scan started there.
// Real R-JPEGs have under 60 markers before the scan (47 at most in the 140 FLIR pictures of the check, 56 for the
// H30T, whose 1280 x 1024 raw image takes 41 APP3 segments) and no fill byte: 4096 markers is over 15 times the 256
// FLIR APP1 segments an FFF file can have (their index is a byte), and 64 KB of fill bytes (0xFF before a marker, which
// the standard allows and no camera writes) is far more than an encoder pads with. A file of tens of MB of RST markers
// or fill bytes is walked in a few thousand reads, not tens of millions of seeks.
constexpr std::size_t kMaxJpegMarkers = 4096;
constexpr std::size_t kMaxJpegFillBytes = 65536;

// The most APP3 segments the raw thermal image of a DJI picture is read from (with payloads; an empty one carries
// nothing and is skipped): the 1280 x 1024 image of the H30T takes 41, the 640 x 512 one of the other cameras 11, and
// kMaxRawThermalPixels (the most their payloads may add up to) is 129 of the 65532 bytes DJI writes in each
constexpr std::size_t kMaxDjiRawThermalChunks = 256;

// The most bytes the FLIR APP1 data of a picture is read from: an FFF file is at most 256 APP1 segments of 64 KB. The
// raw block of a segment without FFF records is looked for in the segments before the scan, from the FLIR one, up to
// this many bytes
constexpr std::size_t kMaxFlirDataBytes = static_cast<std::size_t>(256) * 65536;

// The FFF data of the FLIR APP1 segments of a JPEG file, in the order of their index (thermal.cpp)
std::vector<uint8_t> readFlirFffData(const std::string &filePath);

// The raw counts of the PNG image of a FLIR RawData record of width x height pixels, in the byte order of the record
// (thermal.cpp)
std::optional<std::vector<uint16_t>> decodeFlirPng(const uint8_t *png, std::size_t size, int width, int height,
                                                   bool bigEndian);

// What the R-JPEG reader has done on this thread (test seam): the bytes it asked of files (the probes, the marker walk,
// the FFF data, the APP3 segments, the raw block of a FLIR segment), and the PNG images it started to inflate. Only
// ever increased; a test takes the difference over the calls it checks
struct ThermalReadCounters {
    std::uint64_t fileBytes = 0;
    std::uint64_t pngInflates = 0;
};
const ThermalReadCounters &thermalReadCountersForTesting();

// The bounds of what a point, an area or a value info of a thermal grid holds (raster_analysis.cpp), whatever the size
// of the raster. A raster GDAL reads (ThermalBandReader) is read kThermalGridWindowPixels at a time: the rows of the
// window that fit (a block of rows of the band when they do), or a part of a row wider than that, in the order of the
// pixels, so 4 MB of temperatures and 2 MB of raw counts. The values of an area are kept and sorted, as they always were,
// up to kMaxAreaValuesInMemory (32 MB, 2896 x 2896 pixels with a value: every R-JPEG, whose raw image is at most
// kMaxRawThermalPixels); an area with more is read twice instead: the first pass gives the count, the range, the sum and
// the histogram of the upper 16 bits of the values (ordered as floats), the second the sum of the squared deviations
// and, in the one or two bins that hold the middle ranks, the histograms of the lower 16 bits, which give the median
// exactly (the values sorting puts at those ranks); the sums are compensated (Neumaier). So a request holds at most
// kMaxThermalGridBufferBytes of pixel buffers (39.5 MB), besides the block cache of GDAL and the grid of an R-JPEG
// (at most 24 MB: its raw counts and temperatures), under 64 MB in all.
constexpr std::int64_t kThermalGridWindowPixels = std::int64_t(1) << 20;
constexpr std::size_t kMaxAreaValuesInMemory = std::size_t(1) << 23;
constexpr std::size_t kAreaHistogramBins = 65536;
constexpr std::uint64_t kMaxThermalGridBufferBytes =
    static_cast<std::uint64_t>(kThermalGridWindowPixels) * (sizeof(float) + sizeof(std::uint16_t)) +
    kMaxAreaValuesInMemory * sizeof(float) + 3 * kAreaHistogramBins * sizeof(std::uint64_t);

// The pixel buffers of the thermal grids on this thread (test seam): the bytes they hold now, the most they held at once
// and the largest window a ThermalBandReader read, since the last reset
struct ThermalGridBufferCounters {
    std::uint64_t heldBytes = 0;
    std::uint64_t peakBytes = 0;
    std::uint64_t largestWindowPixels = 0;

    void reset() {
        peakBytes = heldBytes;
        largestWindowPixels = 0;
    }
};
ThermalGridBufferCounters &thermalGridBuffersForTesting();

// The bytes of one pixel buffer, counted in thermalGridBuffersForTesting while it lives: set() after the buffer grows or
// shrinks
class ThermalGridBufferBytes {
public:
    ThermalGridBufferBytes() = default;
    ThermalGridBufferBytes(const ThermalGridBufferBytes &) = delete;
    ThermalGridBufferBytes &operator=(const ThermalGridBufferBytes &) = delete;
    ~ThermalGridBufferBytes();
    void set(std::uint64_t bytes);

private:
    std::uint64_t bytes_ = 0;
};

// The values of an area kept in memory on this thread: kMaxAreaValuesInMemory, or what a test set (the two-pass
// statistics of an area checked on a small raster). The previous limit is back when the override ends
std::size_t areaValuesInMemoryLimit();
class ScopedAreaValuesInMemoryLimitForTesting {
public:
    explicit ScopedAreaValuesInMemoryLimitForTesting(std::size_t limit);
    ScopedAreaValuesInMemoryLimitForTesting(const ScopedAreaValuesInMemoryLimitForTesting &) = delete;
    ScopedAreaValuesInMemoryLimitForTesting &operator=(const ScopedAreaValuesInMemoryLimitForTesting &) = delete;
    ~ScopedAreaValuesInMemoryLimitForTesting();

private:
    std::optional<std::size_t> previous_;
};

} // namespace ddb

#endif // THERMAL_INTERNAL_H
