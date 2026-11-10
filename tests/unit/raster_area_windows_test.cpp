/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The area statistics of a raster the band model reads (getRasterAreaStatsJson, getRasterAreaStatsBoxJson) read the
// rectangle a window at a time into the accumulation of the thermal grids, so that what a request holds does not grow
// with the area (area_statistics_internal.h kMaxAreaBufferBytes): the values of up to kMaxAreaValuesInMemory pixels are
// sorted as before, an area with more is read twice, with the count, the range and the median of the values sorted.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"

#include "area_statistics_internal.h"
#include "exceptions.h"
#include "gdal_inc.h"
#include "json.h"
#include "raster_analysis.h"

namespace
{
    using namespace ddb;

    // A one-band GeoTIFF (EPSG:3857, 4 m pixels) of width x height pixels in tiles of block x block (strips of a row
    // when block is 0), whose rows below writtenRows hold value(x, y) and the others are never written (SPARSE_OK:
    // they read as the NoData value of the band, else 0)
    struct RasterSpec
    {
        int width = 1500;
        int height = 1000;
        int block = 256;
        GDALDataType type = GDT_Float32;
        int writtenRows = -1; // all
        std::function<double(int, int)> value;
        std::optional<double> nodata;
        std::optional<double> scale;
        // A mask of the dataset (0: no value) where it gives false
        std::function<bool(int, int)> mask;
    };

    fs::path makeRaster(const fs::path &path, const RasterSpec &spec)
    {
        char **opts = nullptr;
        if (spec.block > 0)
        {
            opts = CSLSetNameValue(opts, "TILED", "YES");
            opts = CSLSetNameValue(opts, "BLOCKXSIZE", std::to_string(spec.block).c_str());
        }
        opts = CSLSetNameValue(opts, "BLOCKYSIZE", std::to_string(std::max(1, spec.block)).c_str());
        opts = CSLSetNameValue(opts, "SPARSE_OK", "TRUE");
        opts = CSLSetNameValue(opts, "COMPRESS", "DEFLATE");
        GDALDatasetH ds = GDALCreate(GDALGetDriverByName("GTiff"), path.string().c_str(), spec.width, spec.height, 1,
                                     spec.type, opts);
        CSLDestroy(opts);
        if (ds == nullptr)
            throw std::runtime_error("Cannot create the test raster");
        const double gt[6] = {1670000.0, 4.0, 0.0, 5645600.0, 0.0, -4.0};
        GDALSetGeoTransform(ds, const_cast<double *>(gt));
        OGRSpatialReference srs;
        srs.importFromEPSG(3857);
        char *wkt = nullptr;
        srs.exportToWkt(&wkt);
        GDALSetProjection(ds, wkt);
        CPLFree(wkt);

        GDALRasterBandH band = GDALGetRasterBand(ds, 1);
        if (spec.nodata)
            GDALSetRasterNoDataValue(band, *spec.nodata);
        if (spec.scale)
        {
            GDALSetRasterScale(band, *spec.scale);
            GDALSetRasterOffset(band, -40.0);
        }
        const int rows = spec.writtenRows < 0 ? spec.height : spec.writtenRows;
        std::vector<double> row(static_cast<size_t>(spec.width));
        for (int y = 0; y < rows; y++)
        {
            for (int x = 0; x < spec.width; x++)
                row[static_cast<size_t>(x)] = spec.value(x, y);
            EXPECT_EQ(GDALRasterIO(band, GF_Write, 0, y, spec.width, 1, row.data(), spec.width, 1, GDT_Float64, 0, 0),
                      CE_None);
        }
        if (spec.mask)
        {
            EXPECT_EQ(GDALCreateDatasetMaskBand(ds, GMF_PER_DATASET), CE_None);
            GDALRasterBandH mask = GDALGetMaskBand(band);
            std::vector<uint8_t> bytes(static_cast<size_t>(spec.width));
            for (int y = 0; y < spec.height; y++)
            {
                for (int x = 0; x < spec.width; x++)
                    bytes[static_cast<size_t>(x)] = spec.mask(x, y) ? 255 : 0;
                EXPECT_EQ(GDALRasterIO(mask, GF_Write, 0, y, spec.width, 1, bytes.data(), spec.width, 1, GDT_Byte, 0,
                                       0),
                          CE_None);
            }
        }
        GDALClose(ds);
        return path;
    }

    // The values of the pixels of the band read whole that have one (NoData, masked and NaN pixels left out), in
    // physical values, sorted: what the area statistics sorted before
    std::vector<float> sortedValues(const fs::path &path)
    {
        GDALDatasetH ds = GDALOpen(path.string().c_str(), GA_ReadOnly);
        EXPECT_NE(ds, nullptr);
        GDALRasterBandH band = GDALGetRasterBand(ds, 1);
        const int w = GDALGetRasterXSize(ds);
        const int h = GDALGetRasterYSize(ds);
        std::vector<float> samples(static_cast<size_t>(w) * h);
        std::vector<uint8_t> mask(samples.size());
        EXPECT_EQ(GDALRasterIO(band, GF_Read, 0, 0, w, h, samples.data(), w, h, GDT_Float32, 0, 0), CE_None);
        EXPECT_EQ(GDALRasterIO(GDALGetMaskBand(band), GF_Read, 0, 0, w, h, mask.data(), w, h, GDT_Byte, 0, 0),
                  CE_None);
        int hasNodata = 0, hasScale = 0, hasOffset = 0;
        const float nodata = static_cast<float>(GDALGetRasterNoDataValue(band, &hasNodata));
        const double scale = GDALGetRasterScale(band, &hasScale);
        const double offset = GDALGetRasterOffset(band, &hasOffset);
        std::vector<float> values;
        for (size_t i = 0; i < samples.size(); i++)
            if (mask[i] != 0 && std::isfinite(samples[i]) && !(hasNodata && samples[i] == nodata))
                values.push_back(static_cast<float>((hasScale ? samples[i] * scale : samples[i]) +
                                                    (hasOffset ? offset : 0.0)));
        GDALClose(ds);
        std::sort(values.begin(), values.end());
        return values;
    }

    json areaOf(const fs::path &path, int x0, int y0, int x1, int y1)
    {
        return json::parse(getRasterAreaStatsJson(path.string(), x0, y0, x1, y1));
    }

    // The statistics read twice are those of the values kept and sorted: the same count, range, bounds and median,
    // the mean and the standard deviation within the rounding of the last digit of a float
    void expectTheSameStatistics(const json &read, const json &kept)
    {
        SCOPED_TRACE(kept.dump() + " / " + read.dump());
        for (const char *key : {"min", "max", "median", "pixelCount", "bounds", "unit", "isThermal"})
            EXPECT_EQ(read[key], kept[key]) << key;
        EXPECT_NEAR(read["mean"].get<double>(), kept["mean"].get<double>(),
                    2e-6 * std::fabs(kept["mean"].get<double>()));
        EXPECT_NEAR(read["stddev"].get<double>(), kept["stddev"].get<double>(),
                    2e-6 * std::fabs(kept["stddev"].get<double>()));
    }

    // Values over [-100, 400) with ties, both zeros, NaN and the NoData value -9999
    double surfaceValue(int x, int y)
    {
        if (x == 0 && y == 0)
            return std::numeric_limits<double>::quiet_NaN();
        if (x == 1 && y == 0)
            return -9999.0;
        if (x == 2 && y == 0)
            return -0.0;
        if (x == 3 && y == 0)
            return 0.0;
        return std::fmod(x * 0.37 + y * 1.13 + (x * y % 97) * 0.011, 500.0) - 100.0;
    }

    // A Float32 band with NoData and a mask, and a UInt16 band with a scale and an offset (1500 x 1000: two windows of
    // rows): every area has the count, the range and the median of the values sorted, and the same statistics kept
    // in memory and read twice
    TEST(RasterAreaWindows, theAreaOfABandReadInWindowsHasTheStatisticsOfItsValuesSorted)
    {
        TestArea ta(TEST_NAME, true);
        RasterSpec dem;
        dem.value = surfaceValue;
        dem.nodata = -9999.0;
        dem.mask = [](int x, int y) { return !(x >= 700 && x < 720 && y >= 10 && y < 990); };
        RasterSpec counts;
        counts.type = GDT_UInt16;
        counts.value = [](int x, int y) { return static_cast<double>((x * 31 + y * 17) % 4000); };
        counts.scale = 0.01;
        counts.nodata = 0.0;

        const fs::path paths[] = {makeRaster(ta.getPath("dem.tif"), dem), makeRaster(ta.getPath("counts.tif"), counts)};
        for (const fs::path &path : paths)
        {
            SCOPED_TRACE(path.filename().string());
            const std::vector<float> sorted = sortedValues(path);
            ASSERT_GT(sorted.size(), 1000000u);
            const json whole = areaOf(path, 0, 0, 1499, 999);
            EXPECT_EQ(whole["pixelCount"].get<uint64_t>(), sorted.size());
            // The JSON gives the 7 significant digits of a float
            const auto expectFloat = [](const json &value, float expected) {
                EXPECT_NEAR(value.get<double>(), expected, 1e-6 * std::fabs(expected) + 1e-9);
            };
            expectFloat(whole["min"], sorted.front());
            expectFloat(whole["max"], sorted.back());
            const size_t n = sorted.size();
            expectFloat(whole["median"], n % 2 == 0 ? (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0f : sorted[n / 2]);

            const int areas[][4] = {{0, 0, 1499, 999}, {1499, 999, 0, 0}, {7, 600, 1400, 900},
                                    {3, 3, 3, 3},      {-10, -10, 1510, 4}, {690, 0, 730, 999}};
            for (const auto &a : areas)
            {
                const json kept = areaOf(path, a[0], a[1], a[2], a[3]);
                for (const std::size_t limit : {std::size_t(0), std::size_t(1000)})
                {
                    const ScopedAreaValuesInMemoryLimitForTesting readTwice(limit);
                    expectTheSameStatistics(areaOf(path, a[0], a[1], a[2], a[3]), kept);
                }
            }
        }

        // No value: a NoRasterDataException, with the values kept and read twice
        const fs::path dem2 = ta.getPath("dem.tif");
        for (const std::size_t limit : {std::size_t(0), kMaxAreaValuesInMemory})
        {
            const ScopedAreaValuesInMemoryLimitForTesting kept(limit);
            EXPECT_THROW(areaOf(dem2, 1, 0, 1, 0), NoRasterDataException);
            EXPECT_THROW(areaOf(dem2, 705, 20, 715, 30), NoRasterDataException);
        }
    }

    // A sparse Float32 raster of 4096 x 4096 pixels (16.8 million values, over kMaxAreaValuesInMemory), whose 2500
    // first rows have values and whose others read as 0: its whole area, which took 4 bytes a pixel and a sort, is read
    // twice within kMaxAreaBufferBytes of buffers and windows of 2^20 pixels (256 rows, whole blocks), with the
    // statistics of its values kept and sorted, and its WGS84 box gives the same statistics
    TEST(RasterAreaWindows, theAreaOfALargeRasterHoldsBoundedBuffersAndHasTheStatisticsOfItsValuesSorted)
    {
        TestArea ta(TEST_NAME, true);
        constexpr int kSide = 4096;
        RasterSpec spec;
        spec.width = kSide;
        spec.height = kSide;
        spec.writtenRows = 2500;
        spec.value = [](int x, int y) { return std::fmod(x * 0.37 + y * 1.13, 300.0) - 50.0; };
        const fs::path path = makeRaster(ta.getPath("sparse.tif"), spec);
        ASSERT_FALSE(HasFatalFailure());
        constexpr uint64_t kPixels = static_cast<uint64_t>(kSide) * kSide;
        ASSERT_GT(kPixels, kMaxAreaValuesInMemory);

        auto &buffers = areaBuffersForTesting();
        buffers.reset();
        const json read = areaOf(path, 0, 0, kSide - 1, kSide - 1);
        EXPECT_EQ(read["pixelCount"].get<uint64_t>(), kPixels);
        EXPECT_GT(buffers.peakBytes, 0u);
        EXPECT_LE(buffers.peakBytes, kMaxAreaBufferBytes);
        EXPECT_LE(kMaxAreaBufferBytes, 64ull << 20);
        EXPECT_EQ(buffers.largestWindowPixels, static_cast<uint64_t>(kAreaWindowPixels));
        EXPECT_EQ(buffers.heldBytes, 0u) << "every buffer is released";

        // The same raster with its values kept and sorted
        json kept;
        {
            const ScopedAreaValuesInMemoryLimitForTesting all(kPixels);
            kept = areaOf(path, 0, 0, kSide - 1, kSide - 1);
        }
        expectTheSameStatistics(read, kept);
        EXPECT_NE(read["median"].get<double>(), 0.0) << "the median is not the value of the unwritten rows";

        // A WGS84 box around the raster (15.0 to 15.15 E, 44.94 to 45.05 N): the whole area
        const json box = json::parse(getRasterAreaStatsBoxJson(path.string(), 14.9, 44.8, 15.3, 45.3));
        EXPECT_EQ(box["bounds"], read["bounds"]);
        expectTheSameStatistics(box, read);
    }

    // Rows wider than a window are read in parts of a window, in the order of the pixels (strips of a row: tiles of
    // a row that wide would fill the block cache of GDAL with a GB)
    TEST(RasterAreaWindows, rowsWiderThanAWindowAreReadInParts)
    {
        TestArea ta(TEST_NAME, true);
        RasterSpec spec;
        spec.block = 0;
        spec.width = static_cast<int>(kAreaWindowPixels) + 300;
        spec.height = 3;
        spec.writtenRows = 0;
        spec.value = [](int, int) { return 0.0; };
        const fs::path path = makeRaster(ta.getPath("wide.tif"), spec);
        ASSERT_FALSE(HasFatalFailure());

        auto &buffers = areaBuffersForTesting();
        buffers.reset();
        const json kept = areaOf(path, 0, 0, spec.width - 1, 2);
        EXPECT_EQ(kept["pixelCount"], 3 * static_cast<int64_t>(spec.width));
        EXPECT_EQ(kept["median"].get<double>(), 0.0);
        EXPECT_LE(buffers.peakBytes, kMaxAreaBufferBytes);
        EXPECT_EQ(buffers.largestWindowPixels, static_cast<uint64_t>(kAreaWindowPixels));
        EXPECT_EQ(buffers.heldBytes, 0u);

        const ScopedAreaValuesInMemoryLimitForTesting limit(1000);
        expectTheSameStatistics(areaOf(path, 0, 0, spec.width - 1, 2), kept);
    }
}
