/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The raster queries of a place: a point, an area and a stockpile detection that asks for a place
// without data (outside the raster, only no-data pixels, no pile) is a refused argument,
// DDBERR_INVALIDARGS, which a host answers as the caller's error instead of a failure of its own.
// And the histogram of a formula: its bins hold the values their edges hold, over the range of
// the values a viewer shows (the formula's range, narrowed around the 2nd and 98th percentiles).

#include <cmath>
#include <numeric>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"

#include "ddb.h"
#include "json.h"
#include "raster_analysis.h"
#include "raster_test_utils.h"
#include "synthetic_dem_support.h"

namespace
{
    using namespace ddb;

    class RasterQuery : public ::testing::Test
    {
    protected:
        void SetUp() override { DDBRegisterProcess(false); }
    };

    // The output of a successful call, parsed
    json takeJson(char *out)
    {
        json j = json::parse(out);
        DDBFree(out);
        return j;
    }

    // ---- a place without data is a refused argument -----------------------------------------

    // The golden DEM: 70 x 70, with a nodata patch over the pixels (10..14, 50..54)
    fs::path makeDem(TestArea &ta)
    {
        return golden::makeDem(ta.getPath("dem.tif"), golden::demSpecs()[0]);
    }

    TEST_F(RasterQuery, anAreaOfNoDataPixelsOnlyIsARefusedArgument)
    {
        TestArea ta(TEST_NAME, true);
        const std::string dem = makeDem(ta).string();
        char *out = nullptr;
        EXPECT_EQ(DDBGetRasterAreaStats(dem.c_str(), 10, 50, 14, 54, &out), DDBERR_INVALIDARGS);
        EXPECT_EQ(out, nullptr);
        EXPECT_STREQ(DDBGetLastError(), "No valid data in the area: its pixels are all no data");
    }

    TEST_F(RasterQuery, anAreaOutsideTheRasterIsARefusedArgument_NotItsNearestEdge)
    {
        TestArea ta(TEST_NAME, true);
        const std::string dem = makeDem(ta).string();
        char *out = nullptr;
        // Right of the raster: it used to be clamped onto the last column, whose pixels it measured
        EXPECT_EQ(DDBGetRasterAreaStats(dem.c_str(), 80, 10, 90, 20, &out), DDBERR_INVALIDARGS);
        EXPECT_EQ(out, nullptr);
        EXPECT_STREQ(DDBGetLastError(), "No valid data in the area: (80, 10) - (90, 20) is outside the 70x70 raster");
        // Above it, the corners swapped
        EXPECT_EQ(DDBGetRasterAreaStats(dem.c_str(), 20, -5, 10, -20, &out), DDBERR_INVALIDARGS);
        EXPECT_EQ(out, nullptr);
    }

    TEST_F(RasterQuery, anAreaAcrossTheEdgeMeasuresItsPixelsInsideTheRaster)
    {
        TestArea ta(TEST_NAME, true);
        const std::string dem = makeDem(ta).string();
        char *out = nullptr;
        ASSERT_EQ(DDBGetRasterAreaStats(dem.c_str(), 60, -10, 80, 9, &out), DDBERR_NONE) << DDBGetLastError();
        const json j = takeJson(out);
        EXPECT_EQ(j["bounds"], json({{"x0", 60}, {"y0", 0}, {"x1", 69}, {"y1", 9}}));
        EXPECT_EQ(j["pixelCount"], 100);
    }

    TEST_F(RasterQuery, aPointOutsideTheRasterIsARefusedArgument)
    {
        TestArea ta(TEST_NAME, true);
        const std::string dem = makeDem(ta).string();
        char *out = nullptr;
        EXPECT_EQ(DDBGetRasterPointValue(dem.c_str(), 70, 5, &out), DDBERR_INVALIDARGS);
        EXPECT_EQ(out, nullptr);
        EXPECT_STREQ(DDBGetLastError(), "No valid data at the point: pixel (70, 5) is outside the 70x70 raster");
        EXPECT_EQ(DDBGetRasterPointValue(dem.c_str(), 5, -1, &out), DDBERR_INVALIDARGS);

        ASSERT_EQ(DDBGetRasterPointValue(dem.c_str(), 69, 69, &out), DDBERR_NONE) << DDBGetLastError();
        EXPECT_EQ(takeJson(out)["x"], 69);
    }

    // A flat DEM (100 m everywhere) in UTM: nothing stands out of it
    const golden::DemSpec kFlat{"flat", 32632, 578000.0, 4985000.0, 10.0, 70};

    fs::path makeFlatDem(TestArea &ta)
    {
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = kFlat.size;
        spec.bands = 1;
        spec.type = GDT_Float32;
        spec.epsg = kFlat.epsg;
        spec.originX = kFlat.originX;
        spec.originY = kFlat.originY;
        spec.pixelSize = kFlat.pixel;
        spec.value = [](int, int, int) { return 100.0; };
        return testutils::createGeoRaster(ta.getPath("flat.tif"), spec);
    }

    TEST_F(RasterQuery, aStockpileDetectionThatFindsNoPileIsARefusedArgument)
    {
        TestArea ta(TEST_NAME, true);
        const std::string dem = makeFlatDem(ta).string();
        const auto [lon, lat] = golden::lonLatOfPixel(kFlat, 35.5, 35.5);
        char *out = nullptr;
        EXPECT_EQ(DDBDetectStockpile(dem.c_str(), lat, lon, 100.0, 0.5f, &out), DDBERR_INVALIDARGS);
        EXPECT_EQ(out, nullptr);
        EXPECT_STREQ(DDBGetLastError(), "No stockpile detected at this location");

        // A full scan of the same DEM finds none either: that is an empty list
        ASSERT_EQ(DDBDetectAllStockpiles(dem.c_str(), 0.5f, 0.0, 10, &out), DDBERR_NONE) << DDBGetLastError();
        takeJson(out);
    }

    TEST_F(RasterQuery, aStockpileDetectionOutsideTheRasterIsARefusedArgument)
    {
        TestArea ta(TEST_NAME, true);
        const std::string dem = makeFlatDem(ta).string();
        const auto [lon, lat] = golden::lonLatOfPixel(kFlat, 200.5, 35.5);
        char *out = nullptr;
        EXPECT_EQ(DDBDetectStockpile(dem.c_str(), lat, lon, 100.0, 0.5f, &out), DDBERR_INVALIDARGS);
        EXPECT_STREQ(DDBGetLastError(), "Click point is outside raster bounds");
    }

    TEST_F(RasterQuery, aStockpileDetectionOverNoDataIsARefusedArgument)
    {
        TestArea ta(TEST_NAME, true);
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = kFlat.size;
        spec.bands = 1;
        spec.type = GDT_Float32;
        spec.epsg = kFlat.epsg;
        spec.originX = kFlat.originX;
        spec.originY = kFlat.originY;
        spec.pixelSize = kFlat.pixel;
        spec.value = [](int, int, int) { return golden::kNodata; };
        const fs::path empty = testutils::createGeoRaster(ta.getPath("empty.tif"), spec);
        {
            GDALDatasetH ds = GDALOpen(empty.string().c_str(), GA_Update);
            ASSERT_NE(ds, nullptr);
            GDALSetRasterNoDataValue(GDALGetRasterBand(ds, 1), golden::kNodata);
            GDALClose(ds);
        }
        const auto [lon, lat] = golden::lonLatOfPixel(kFlat, 35.5, 35.5);
        char *out = nullptr;
        EXPECT_EQ(DDBDetectStockpile(empty.string().c_str(), lat, lon, 100.0, 0.5f, &out), DDBERR_INVALIDARGS);
        EXPECT_STREQ(DDBGetLastError(), "Search region contains no valid data");
        EXPECT_EQ(DDBDetectAllStockpiles(empty.string().c_str(), 0.5f, 0.0, 10, &out), DDBERR_INVALIDARGS);
        EXPECT_STREQ(DDBGetLastError(), "Raster contains no valid elevation data");
    }

    // ---- the histogram of a formula ----------------------------------------------------------

    TEST(FormulaHistogram, theBinOfAValueIsTheOneWhoseEdgesHoldIt)
    {
        std::vector<double> values;
        for (int i = 0; i < 10; i++)
            values.push_back(i + 0.5);
        values.push_back(0.0);   // the first edge: the first bin
        values.push_back(10.0);  // the last edge: the last bin
        values.push_back(-0.1);  // outside
        values.push_back(10.1);
        values.push_back(42.0);

        const ValueHistogram h = binValues(values, 0.0, 10.0, 10);
        ASSERT_EQ(h.edges.size(), 11u);
        EXPECT_DOUBLE_EQ(h.edges.front(), 0.0);
        EXPECT_DOUBLE_EQ(h.edges.back(), 10.0);
        EXPECT_EQ(h.counts, (std::vector<int>{2, 1, 1, 1, 1, 1, 1, 1, 1, 2}));
        EXPECT_EQ(h.below, 1);
        EXPECT_EQ(h.above, 2);
    }

    TEST(FormulaHistogram, theRangeIsTheFormulasRangeNarrowedAroundThePercentiles)
    {
        using Range = std::pair<double, double>;
        const std::optional<Range> ndvi = Range{-1.0, 1.0};
        // The micasense NDVI: outliers far outside [-1, 1], the bulk between 0.37 and 0.95
        const Range r = formulaHistogramRange(-191.7, 278.5, 0.37, 0.95, ndvi);
        EXPECT_DOUBLE_EQ(r.first, 0.37 - 0.58);
        EXPECT_DOUBLE_EQ(r.second, 1.0);
        // No outliers: the values' own range
        EXPECT_EQ(formulaHistogramRange(0.1, 0.8, 0.2, 0.7, ndvi), Range(0.1, 0.8));
        // A formula without a range: the percentiles only
        EXPECT_EQ(formulaHistogramRange(-50.0, 50.0, 1.0, 2.0, std::nullopt), Range(0.0, 3.0));
        // All the values outside the formula's range: their own range
        EXPECT_EQ(formulaHistogramRange(3.0, 5.0, 3.5, 4.5, ndvi), Range(3.0, 5.0));
        // One value
        EXPECT_EQ(formulaHistogramRange(0.4, 0.4, 0.4, 0.4, ndvi), Range(0.4, 1.4));
    }

    // A five-band raster (R, G, B, RedEdge, NIR) of 128 x 128 whose NIR and red make the NDVI
    // of `ndvi(x, y)`: red r and NIR n with (n - r) / (n + r) = v for n + r = 1
    template <typename F>
    fs::path makeNdviRaster(TestArea &ta, const std::string &name, F ndvi)
    {
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = 128;
        spec.bands = 5;
        spec.type = GDT_Float32;
        spec.epsg = 3857;
        spec.value = [ndvi](int band, int x, int y) {
            const double v = ndvi(x, y);
            if (band == 1) return (1.0 - v) / 2.0;
            if (band == 5) return (1.0 + v) / 2.0;
            return 0.2;
        };
        return testutils::createGeoRaster(ta.getPath(name), spec);
    }

    json ndviStatistics(const fs::path &raster)
    {
        char *out = nullptr;
        EXPECT_EQ(DDBGetRasterMetadata(raster.string().c_str(), "NDVI", "RGBReN", &out), DDBERR_NONE)
            << DDBGetLastError();
        return takeJson(out)["statistics"]["1"];
    }

    // The bin whose edges hold v
    size_t binOf(const json &edges, double v)
    {
        const size_t bins = edges.size() - 1;
        for (size_t i = 0; i < bins; i++)
            if (v >= edges[i].get<double>() && (v < edges[i + 1].get<double>() || i == bins - 1))
                return i;
        return bins;
    }

    TEST_F(RasterQuery, theHistogramOfAFormulaCountsEachValueInTheBinWhoseEdgesHoldIt)
    {
        TestArea ta(TEST_NAME, true);
        // Four values, a quarter of the pixels each. 0.6842 is 0.912 of the range: binned with
        // 254 steps over edges of 255 bins it fell one bin low
        const std::vector<double> quarter = {0.0, 0.375, 0.6842, 0.75};
        const fs::path raster = makeNdviRaster(ta, "quarters.tif", [&](int x, int y) {
            return quarter[(x < 64 ? 0 : 1) + (y < 64 ? 0 : 2)];
        });
        const json stats = ndviStatistics(raster);
        const json &h = stats["histogram"];
        const json &edges = h["edges"];
        ASSERT_EQ(edges.size(), 256u);
        EXPECT_NEAR(edges.front().get<double>(), 0.0, 1e-6);
        EXPECT_NEAR(edges.back().get<double>(), 0.75, 1e-6);

        std::vector<int> expected(255, 0);
        for (const double v : quarter)
        {
            // The values as the formula computes them, in float
            const float r = static_cast<float>((1.0 - v) / 2.0), n = static_cast<float>((1.0 + v) / 2.0);
            const float f = (n - r) / (n + r);
            expected[binOf(edges, f)] += 64 * 64;
        }
        EXPECT_EQ(h["counts"].get<std::vector<int>>(), expected);
        EXPECT_EQ(h["below"], 0);
        EXPECT_EQ(h["above"], 0);
    }

    TEST_F(RasterQuery, theHistogramOfAFormulaWithOutliersBinsTheRangeAViewerShows)
    {
        TestArea ta(TEST_NAME, true);
        // Two columns of outliers on each side (1.6% of the pixels each), the bulk from 0.3 to 0.9
        const fs::path raster = makeNdviRaster(ta, "outliers.tif", [](int x, int y) {
            if (x < 2) return -40.0;
            if (x >= 126) return 60.0;
            return 0.3 + 0.6 * ((x * 7 + y * 3) % 100) / 99.0;
        });
        const json stats = ndviStatistics(raster);
        const json &h = stats["histogram"];
        const double p2 = stats["percentiles"]["p2"], p98 = stats["percentiles"]["p98"];
        const double spread = p98 - p2;

        // min and max are those of all the values
        EXPECT_NEAR(stats["min"].get<double>(), -40.0, 1e-3);
        EXPECT_NEAR(stats["max"].get<double>(), 60.0, 1e-3);
        // The bins span the formula's range [-1, 1] within one spread of the percentiles
        const double lo = h["edges"].front(), hi = h["edges"].back();
        EXPECT_NEAR(lo, std::max(-1.0, p2 - spread), 1e-9);
        EXPECT_NEAR(hi, std::min(1.0, p98 + spread), 1e-9);
        // Every value is in a bin or counted outside them
        const auto counts = h["counts"].get<std::vector<int>>();
        EXPECT_EQ(h["below"], 2 * 128);
        EXPECT_EQ(h["above"], 2 * 128);
        EXPECT_EQ(std::accumulate(counts.begin(), counts.end(), 0), 124 * 128);
        // The bulk fills the bins: no bar holds most of the values
        EXPECT_LT(*std::max_element(counts.begin(), counts.end()), 124 * 128 / 10);
    }
}
