/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The bounded sample of a raster (planBoundedSample): what the stretch of a thumbnail reads and what a formula export
// without a rescale and without a range of its formula takes its automatic range from, the 2nd to the 98th percentile
// of the formula over the sample. A part of at most the budget is read whole (the range of every pixel, as before);
// a larger one is read in the largest overview within the budget, else in whole blocks on an even lattice, so the
// export does not compute the formula over the whole raster twice.

#include <cstdint>
#include <functional>
#include <optional>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "json.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

#include "ddb.h"
#include "gdal_inc.h"
#include "raster_export.h"
#include "raster_stats.h"
#include "raster_utils.h"
#include "vegetation.h"

namespace
{
    using namespace ddb;

    class BoundedSampleTest : public ::testing::Test
    {
    protected:
        void SetUp() override { DDBRegisterProcess(false); }
    };

    // A tiled Float32 GeoTIFF of width x height with `bands` bands whose band b holds value(b, x, y) (1-based), in
    // blocks of block x block, with the overviews `levels` (nearest) when there are
    void writeRaster(const fs::path &path, int width, int height, int bands, int block,
                     const std::function<double(int, int, int)> &value, const std::vector<int> &levels = {},
                     std::optional<double> nodata = std::nullopt)
    {
        CPLStringList options;
        options.AddString("TILED=YES");
        options.AddString(("BLOCKXSIZE=" + std::to_string(block)).c_str());
        options.AddString(("BLOCKYSIZE=" + std::to_string(block)).c_str());
        options.AddString("COMPRESS=DEFLATE");
        std::unique_ptr<GDALDataset> ds(GetGDALDriverManager()->GetDriverByName("GTiff")->Create(
            path.string().c_str(), width, height, bands, GDT_Float32, options.List()));
        ASSERT_NE(ds, nullptr);
        const double gt[6] = {500000.0, 0.1, 0.0, 5000000.0, 0.0, -0.1};
        ds->SetGeoTransform(const_cast<double *>(gt));
        OGRSpatialReference srs;
        srs.importFromEPSG(32633);
        ds->SetSpatialRef(&srs);
        std::vector<float> row(width);
        for (int b = 1; b <= bands; b++)
        {
            if (nodata)
                ds->GetRasterBand(b)->SetNoDataValue(*nodata);
            for (int y = 0; y < height; y++)
            {
                for (int x = 0; x < width; x++)
                    row[x] = static_cast<float>(value(b, x, y));
                ASSERT_EQ(ds->GetRasterBand(b)->RasterIO(GF_Write, 0, y, width, 1, row.data(), width, 1, GDT_Float32,
                                                         0, 0, nullptr),
                          CE_None);
            }
        }
        if (!levels.empty())
        {
            ASSERT_EQ(ds->BuildOverviews("NEAREST", static_cast<int>(levels.size()), levels.data(), 0, nullptr,
                                         nullptr, nullptr),
                      CE_None);
        }
    }

    std::unique_ptr<GDALDataset> open(const fs::path &path, GDALAccess access = GA_ReadOnly)
    {
        return std::unique_ptr<GDALDataset>(static_cast<GDALDataset *>(GDALOpen(path.string().c_str(), access)));
    }

    std::vector<RasterWindow> windowsOf(const BoundedSample &sample, int maxSide)
    {
        std::vector<RasterWindow> windows;
        sample.forEachWindow(maxSide, [&](const RasterWindow &w) { windows.push_back(w); });
        return windows;
    }

    std::string text(const RasterWindow &w)
    {
        return std::to_string(w.x) + "," + std::to_string(w.y) + " " + std::to_string(w.xsize) + "x" +
               std::to_string(w.ysize) + " -> " + std::to_string(w.outWidth) + "x" + std::to_string(w.outHeight);
    }

    std::vector<std::string> texts(const std::vector<RasterWindow> &windows)
    {
        std::vector<std::string> out;
        for (const RasterWindow &w : windows)
            out.push_back(text(w));
        return out;
    }

    const auto anyValue = [](int, int x, int y) { return static_cast<double>(x + y); };

    // A part within the budget is read whole, in the windows of a grid from its corner: every pixel once
    TEST_F(BoundedSampleTest, aPartWithinTheBudgetIsReadWhole)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path path = ta.getPath("r.tif");
        writeRaster(path, 100, 60, 2, 16, anyValue);
        const auto ds = open(path);
        ASSERT_NE(ds, nullptr);

        const BoundedSample sample =
            planBoundedSample(GDALDataset::ToHandle(ds.get()), {1, 2}, RasterWindow{0, 0, 100, 60, 100, 60}, 6000);
        EXPECT_EQ(sample.kind, BoundedSample::Kind::Whole);
        EXPECT_FALSE(sample.sampled());
        EXPECT_EQ(texts(windowsOf(sample, 64)),
                  (std::vector<std::string>{"0,0 64x60 -> 64x60", "64,0 36x60 -> 36x60"}));

        const BoundedSample part =
            planBoundedSample(GDALDataset::ToHandle(ds.get()), {1, 2}, RasterWindow{30, 20, 50, 33, 50, 33}, 1650);
        EXPECT_EQ(part.kind, BoundedSample::Kind::Whole);
        EXPECT_EQ(texts(windowsOf(part, 32)),
                  (std::vector<std::string>{"30,20 32x32 -> 32x32", "62,20 18x32 -> 18x32", "30,52 32x1 -> 32x1",
                                            "62,52 18x1 -> 18x1"}));
    }

    // Without overviews a larger part is read in whole blocks of the raster, the middle block of each cell of an even
    // lattice, cut to the part (and in windows of at most maxSide)
    TEST_F(BoundedSampleTest, aLargerPartIsReadInTheBlocksOfALattice)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path path = ta.getPath("r.tif");
        writeRaster(path, 100, 60, 2, 16, anyValue);
        const auto ds = open(path);
        ASSERT_NE(ds, nullptr);
        GDALDatasetH handle = GDALDataset::ToHandle(ds.get());

        // 7 x 4 blocks of 256 pixels, at most 3 of them: a step of 4, the blocks 2 and 6 of row 2
        const BoundedSample whole = planBoundedSample(handle, {1, 2}, RasterWindow{0, 0, 100, 60, 100, 60}, 1000);
        EXPECT_EQ(whole.kind, BoundedSample::Kind::Blocks);
        EXPECT_TRUE(whole.sampled());
        EXPECT_EQ(whole.step, 4u);
        EXPECT_EQ(texts(windowsOf(whole, 64)),
                  (std::vector<std::string>{"32,32 16x16 -> 16x16", "96,32 4x16 -> 4x16"}));
        EXPECT_EQ(texts(windowsOf(whole, 10)),
                  (std::vector<std::string>{"32,32 10x10 -> 10x10", "42,32 6x10 -> 6x10", "32,42 10x6 -> 10x6",
                                            "42,42 6x6 -> 6x6", "96,32 4x10 -> 4x10", "96,42 4x6 -> 4x6"}));

        // A part that starts inside a block: the 4 x 3 blocks it touches, one of them, cut to the part
        const BoundedSample part = planBoundedSample(handle, {1, 2}, RasterWindow{10, 5, 50, 40, 50, 40}, 500);
        EXPECT_EQ(part.kind, BoundedSample::Kind::Blocks);
        EXPECT_EQ(part.blocksX, 4u);
        EXPECT_EQ(part.blocksY, 3u);
        EXPECT_EQ(texts(windowsOf(part, 64)), (std::vector<std::string>{"32,32 16x13 -> 16x13"}));

        // A part that starts on a later block counts its blocks from there
        const BoundedSample later = planBoundedSample(handle, {1, 2}, RasterWindow{40, 20, 60, 40, 60, 40}, 600);
        EXPECT_EQ(later.firstBlockX, 2u);
        EXPECT_EQ(later.firstBlockY, 1u);
        for (const RasterWindow &w : windowsOf(later, 64))
        {
            EXPECT_GE(w.x, 40) << text(w);
            EXPECT_GE(w.y, 20) << text(w);
            EXPECT_LE(w.x + w.xsize, 100) << text(w);
            EXPECT_LE(w.y + w.ysize, 60) << text(w);
        }
    }

    // With overviews a larger part is read in the largest overview in which it is within the budget, at the size it
    // has there: windows whose output is smaller than their pixels, which cover the part once
    TEST_F(BoundedSampleTest, aLargerPartIsReadInTheLargestOverviewWithinTheBudget)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path path = ta.getPath("r.tif");
        writeRaster(path, 100, 60, 2, 16, anyValue, {2, 4});
        const auto ds = open(path);
        ASSERT_NE(ds, nullptr);
        GDALDatasetH handle = GDALDataset::ToHandle(ds.get());

        // 50 x 30 and 25 x 15
        const BoundedSample two = planBoundedSample(handle, {1, 2}, RasterWindow{0, 0, 100, 60, 100, 60}, 2000);
        EXPECT_EQ(two.kind, BoundedSample::Kind::Overview);
        EXPECT_EQ(two.overview, 0);
        EXPECT_EQ(two.outWidth, 50);
        EXPECT_EQ(two.outHeight, 30);
        // Windows over at most 32 x 32 pixels of the raster: 16 x 16 of the overview
        EXPECT_EQ(texts(windowsOf(two, 32)),
                  (std::vector<std::string>{"0,0 32x32 -> 16x16", "32,0 32x32 -> 16x16", "64,0 32x32 -> 16x16",
                                            "96,0 4x32 -> 2x16", "0,32 32x28 -> 16x14", "32,32 32x28 -> 16x14",
                                            "64,32 32x28 -> 16x14", "96,32 4x28 -> 2x14"}));
        const BoundedSample four = planBoundedSample(handle, {1, 2}, RasterWindow{0, 0, 100, 60, 100, 60}, 1000);
        EXPECT_EQ(four.overview, 1);
        EXPECT_EQ(four.outWidth, 25);

        // The part 40 x 30 is 20 x 15 in the first overview
        const BoundedSample part = planBoundedSample(handle, {1, 2}, RasterWindow{10, 6, 40, 30, 40, 30}, 400);
        EXPECT_EQ(part.kind, BoundedSample::Kind::Overview);
        EXPECT_EQ(part.overview, 0);
        EXPECT_EQ(part.outWidth, 20);
        EXPECT_EQ(part.outHeight, 15);
        size_t outPixels = 0, pixels = 0;
        for (const RasterWindow &w : windowsOf(part, 8))
        {
            EXPECT_LE(w.xsize, 8) << text(w);
            EXPECT_LE(w.ysize, 8) << text(w);
            EXPECT_GE(w.x, 10) << text(w);
            EXPECT_GE(w.y, 6) << text(w);
            EXPECT_LE(w.x + w.xsize, 50) << text(w);
            EXPECT_LE(w.y + w.ysize, 36) << text(w);
            outPixels += w.outPixels();
            pixels += static_cast<size_t>(w.xsize) * w.ysize;
        }
        EXPECT_EQ(outPixels, 300u);
        EXPECT_EQ(pixels, 1200u);

        // A band that the raster does not have
        EXPECT_THROW(planBoundedSample(handle, {3}, RasterWindow{0, 0, 100, 60, 100, 60}, 1000), GDALException);
    }

    // Without an overview within the budget, the decimated fallback reads the window at the largest size of its
    // proportions within the budget, in windows over at most maxSide x maxSide pixels; the default fallback reads
    // blocks
    TEST_F(BoundedSampleTest, theDecimatedFallbackReadsAnEvenGridOfTheWholeWindow)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path path = ta.getPath("r.tif");
        writeRaster(path, 100, 60, 2, 16, anyValue, {8});
        const auto ds = open(path);
        ASSERT_NE(ds, nullptr);
        GDALDatasetH handle = GDALDataset::ToHandle(ds.get());
        const RasterWindow whole{0, 0, 100, 60, 100, 60};

        // An overview within the budget (13 x 8) is read before the fallback
        EXPECT_EQ(planBoundedSample(handle, {1, 2}, whole, 1500, BoundedSample::Fallback::Decimated).kind,
                  BoundedSample::Kind::Overview);

        const BoundedSample blocks = planBoundedSample(handle, {1, 2}, whole, 100);
        EXPECT_EQ(blocks.kind, BoundedSample::Kind::Blocks);
        const BoundedSample decimated = planBoundedSample(handle, {1, 2}, whole, 100, BoundedSample::Fallback::Decimated);
        EXPECT_EQ(decimated.kind, BoundedSample::Kind::Decimated);
        EXPECT_TRUE(decimated.sampled());
        // sqrt(100 / 6000): 12 x 7
        EXPECT_EQ(decimated.outWidth, 12);
        EXPECT_EQ(decimated.outHeight, 7);
        size_t outPixels = 0, pixels = 0;
        for (const RasterWindow &w : windowsOf(decimated, 50))
        {
            EXPECT_LE(w.xsize, 50) << text(w);
            EXPECT_LE(w.ysize, 50) << text(w);
            outPixels += w.outPixels();
            pixels += static_cast<size_t>(w.xsize) * w.ysize;
        }
        EXPECT_EQ(outPixels, 84u);
        EXPECT_EQ(pixels, 6000u);

        // A long and narrow window is read in one row at least, within the budget
        const BoundedSample narrow =
            planBoundedSample(handle, {1, 2}, RasterWindow{0, 10, 100, 2, 100, 2}, 10, BoundedSample::Fallback::Decimated);
        EXPECT_EQ(narrow.kind, BoundedSample::Kind::Decimated);
        EXPECT_EQ(narrow.outWidth, 10);
        EXPECT_EQ(narrow.outHeight, 1);
    }

    class FormulaExportRange : public ::testing::Test
    {
    protected:
        void SetUp() override { DDBRegisterProcess(false); }
    };

    // The rasters of the export tests are larger than the budget of 2048 x 2048 pixels: 2400 x 2400 in blocks of
    // 400 with overviews, and 4096 x 4096 without, which the sample reads decimated by 2 (the pixel 2i + 1 of
    // every 2 x 2 cell, the nearest to its centre)
    constexpr int kSize = 2400;
    constexpr int kBlock = 400;
    constexpr int kLarge = 4096;

    bool inDecimatedSample(int x, int y)
    {
        return x % 2 == 1 && y % 2 == 1;
    }

    // An export of GRVI (N / G, a formula without a range of its own) with the band G at 1, so its values are those
    // of the band N; the raster is an artefact of the cache, which the export reads as it is
    struct GrviExport
    {
        TestArea ta;
        ScopedProcessConfig cache;
        fs::path input;

        explicit GrviExport(const std::string &name)
            : ta(name, true), cache(json{{"cacheDir", ta.getPath("cache").string()}}.dump()),
              input(ta.getPath("cache/artefacts/grvi.tif"))
        {
            fs::create_directories(input.parent_path());
        }

        // The export (RGBA) of the whole raster with a tile of tileSize
        std::unique_ptr<GDALDataset> run(int tileSize = 0)
        {
            RasterExport request;
            request.input = input.string();
            request.output = ta.getPath("out-" + std::to_string(tileSize) + ".tif").string();
            request.formula = "GRVI";
            request.bandFilter = "GN";
            request.colormap = "rdylgn";
            request.tileSize = tileSize;
            exportRaster(request);
            return open(request.output);
        }
    };

    ColormapEntry colourAt(GDALDataset &out, int x, int y)
    {
        uint8_t rgba[4] = {};
        for (int b = 0; b < 4; b++)
            EXPECT_EQ(out.GetRasterBand(b + 1)->RasterIO(GF_Read, x, y, 1, 1, &rgba[b], 1, 1, GDT_Byte, 0, 0, nullptr),
                      CE_None);
        return {rgba[0], rgba[1], rgba[2], rgba[3]};
    }

    void expectColour(GDALDataset &out, int x, int y, int entry)
    {
        const ColormapEntry expected = VegetationEngine::instance().getColormap("rdylgn")->entries[entry];
        const ColormapEntry got = colourAt(out, x, y);
        EXPECT_EQ(got.r, expected.r) << x << "," << y << " entry " << entry;
        EXPECT_EQ(got.g, expected.g) << x << "," << y << " entry " << entry;
        EXPECT_EQ(got.b, expected.b) << x << "," << y << " entry " << entry;
        EXPECT_EQ(got.a, expected.a) << x << "," << y;
    }

    // The range of a large export without overviews is that of its decimated sample: the values of the pixels it
    // reads go from 0.1 to 0.3, those of the others are all 0.9. Over the sample, 0.3 is above the 98th percentile
    // (the last colour) and 0.1 below the 2nd (the first); over every pixel, as the export took it before, 0.9 was
    // the 98th percentile and 0.3 a colour about a quarter up the ramp.
    TEST_F(FormulaExportRange, aLargeExportTakesItsRangeFromItsDecimatedSample)
    {
        GrviExport e(TEST_NAME);
        writeRaster(e.input, kLarge, kLarge, 2, 512, [](int b, int x, int y) {
            if (b == 1)
                return 1.0;
            return inDecimatedSample(x, y) ? 0.1 + 0.2 * ((x / 2 + y / 2) % 101) / 100.0 : 0.9;
        });
        ASSERT_FALSE(HasFatalFailure());
        const auto out = e.run();
        ASSERT_NE(out, nullptr);
        expectColour(*out, 1, 1, 0);     // 0.1
        expectColour(*out, 201, 1, 255); // 0.3
        expectColour(*out, 0, 0, 255);   // 0.9
        expectColour(*out, 4094, 4095, 255); // 0.9
    }

    // With overviews the sample is the largest overview within the budget (1200 x 1200 here), read as the overview
    // holds it: its values (0.2 to 0.4, written into it on purpose) give the range, and every pixel of the raster
    // (0.6 to 0.8) is above it. The range does not depend on the tile of the export.
    TEST_F(FormulaExportRange, aLargeExportWithOverviewsTakesItsRangeFromTheOverview)
    {
        GrviExport e(TEST_NAME);
        const auto ramp = [](int x, int y, double low) { return low + 0.2 * ((x + y) % 101) / 100.0; };
        writeRaster(
            e.input, kSize, kSize, 2, kBlock, [&](int b, int x, int y) { return b == 1 ? 1.0 : ramp(x, y, 0.6); },
            {2, 4});
        ASSERT_FALSE(HasFatalFailure());
        {
            const auto ds = open(e.input, GA_Update);
            ASSERT_NE(ds, nullptr);
            GDALRasterBand *overview = ds->GetRasterBand(2)->GetOverview(0);
            ASSERT_NE(overview, nullptr);
            ASSERT_EQ(overview->GetXSize(), kSize / 2);
            std::vector<float> row(kSize / 2);
            for (int y = 0; y < kSize / 2; y++)
            {
                for (int x = 0; x < kSize / 2; x++)
                    row[x] = static_cast<float>(ramp(x, y, 0.2));
                ASSERT_EQ(overview->RasterIO(GF_Write, 0, y, kSize / 2, 1, row.data(), kSize / 2, 1, GDT_Float32, 0,
                                             0, nullptr),
                          CE_None);
            }
        }
        const auto out = e.run(256);
        ASSERT_NE(out, nullptr);
        // Over the raster the lowest values (0.6, at x + y = 0, 101, ...) would take the first colour
        for (const auto &[x, y] : std::vector<std::pair<int, int>>{{0, 0}, {101, 0}, {50, 51}, {1234, 2000}, {2399, 2399}})
            expectColour(*out, x, y, 255);

        const auto other = e.run(1024);
        ASSERT_NE(other, nullptr);
        for (int b = 1; b <= 4; b++)
        {
            std::vector<uint8_t> a(static_cast<size_t>(kSize) * 64), c(a.size());
            ASSERT_EQ(out->GetRasterBand(b)->RasterIO(GF_Read, 0, 1000, kSize, 64, a.data(), kSize, 64, GDT_Byte, 0, 0,
                                                      nullptr),
                      CE_None);
            ASSERT_EQ(other->GetRasterBand(b)->RasterIO(GF_Read, 0, 1000, kSize, 64, c.data(), kSize, 64, GDT_Byte, 0,
                                                        0, nullptr),
                      CE_None);
            EXPECT_EQ(a, c) << "band " << b;
        }
    }

    // A sample in which no pixel has a value (every pixel it reads is NoData) is read again with a larger budget,
    // here every pixel: the range is that of the values (2 to 3), not the nominal 0 to 1 of no value
    TEST_F(FormulaExportRange, aSampleWithoutAValueIsReadAgainWithALargerBudget)
    {
        GrviExport e(TEST_NAME);
        writeRaster(
            e.input, kLarge, kLarge, 2, 512,
            [](int b, int x, int y) {
                if (inDecimatedSample(x, y))
                    return -1.0;
                return b == 1 ? 1.0 : 2.0 + ((x + y) % 101) / 100.0;
            },
            {}, -1.0);
        ASSERT_FALSE(HasFatalFailure());
        const auto out = e.run();
        ASSERT_NE(out, nullptr);
        expectColour(*out, 0, 0, 0);       // 2.0
        expectColour(*out, 100, 0, 255);   // 3.0
        EXPECT_EQ(colourAt(*out, 1, 1).a, 0); // NoData
    }
}
