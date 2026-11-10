/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef AREA_STATISTICS_INTERNAL_H
#define AREA_STATISTICS_INTERNAL_H

// Private to the area statistics and the value info of the raster API (raster_analysis.cpp), the windowed reader of a
// thermal band (thermal.cpp) and their unit tests: not installed, not part of the API.

#include <cstddef>
#include <cstdint>
#include <optional>

namespace ddb {

// The bounds of what the statistics of an area (getRasterAreaStatsJson, getRasterAreaStatsBoxJson, the thermal grids)
// and the value info of a thermal grid hold, whatever the size of the raster. A raster GDAL reads is read
// kAreaWindowPixels at a time: the rows of the area that fit (a block of rows of the band when they do), or a part of
// a row wider than that, in the order of the pixels, so 4 MB of values and at most 2 bytes a pixel beside them (the
// raw counts of a thermal band, the mask of a band). The values of an area are kept and sorted, as they always were,
// up to kMaxAreaValuesInMemory (32 MB, 2896 x 2896 pixels with a value: every R-JPEG, whose raw image is at most
// kMaxRawThermalPixels); an area with more is read twice instead: the first pass gives the count, the range, the sum
// and the histogram of the upper 16 bits of the values (ordered as floats), the second the sum of the squared
// deviations and, in the one or two bins that hold the middle ranks, the histograms of the lower 16 bits, which give
// the median exactly (the values sorting puts at those ranks); the sums are compensated (Neumaier). So a request holds
// at most kMaxAreaBufferBytes of pixel buffers (39.5 MB), besides the block cache of GDAL and the grid of an R-JPEG
// (at most 24 MB: its raw counts and temperatures), under 64 MB in all.
constexpr std::int64_t kAreaWindowPixels = std::int64_t(1) << 20;
constexpr std::size_t kMaxAreaValuesInMemory = std::size_t(1) << 23;
constexpr std::size_t kAreaHistogramBins = 65536;
constexpr std::uint64_t kMaxAreaBufferBytes =
    static_cast<std::uint64_t>(kAreaWindowPixels) * (sizeof(float) + sizeof(std::uint16_t)) +
    kMaxAreaValuesInMemory * sizeof(float) + 3 * kAreaHistogramBins * sizeof(std::uint64_t);

// The pixel buffers of the area statistics and the thermal grids on this thread (test seam): the bytes they hold now,
// the most they held at once and the largest window read (countAreaWindow), since the last reset
struct AreaBufferCounters {
    std::uint64_t heldBytes = 0;
    std::uint64_t peakBytes = 0;
    std::uint64_t largestWindowPixels = 0;

    void reset() {
        peakBytes = heldBytes;
        largestWindowPixels = 0;
    }
};
AreaBufferCounters &areaBuffersForTesting();

// A window of that many pixels is read on this thread (counted in areaBuffersForTesting)
void countAreaWindow(std::uint64_t pixels);

// The bytes of one pixel buffer, counted in areaBuffersForTesting while it lives: set() after the buffer grows or
// shrinks
class AreaBufferBytes {
public:
    AreaBufferBytes() = default;
    AreaBufferBytes(const AreaBufferBytes &) = delete;
    AreaBufferBytes &operator=(const AreaBufferBytes &) = delete;
    ~AreaBufferBytes();
    void set(std::uint64_t bytes);

private:
    std::uint64_t bytes_ = 0;
};

// The values of an area kept in memory on this thread: kMaxAreaValuesInMemory, or what a test set (the two-pass
// statistics of an area checked on a small raster, the sorted ones on a large one). The previous limit is back when
// the override ends
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

#endif // AREA_STATISTICS_INTERNAL_H
