/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-06: one nodata rule. A band's nodata value is matched in the precision of the samples
// (float32 for the analyses), and a NaN nodata value matches NaN samples only. Before DQ-06 the
// analyses compared a float sample, widened to double, with the double nodata value within
// 1e-9 (volume, stockpile), 1e-6 (profile, area statistics) or 1e-3 (align), the formulas
// exactly in double: a nodata value that float32 cannot hold (-9999.9) was never matched, so
// nodata pixels were counted as elevations, and a NaN nodata value switched off the elevation
// offset of align and made the index render transparent everywhere.

#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"
#include "raster_test_utils.h"

#include "align.h"
#include "ddb.h"
#include "dq06_dem_support.h"
#include "dq06_golden_support.h"
#include "dq06_raster_support.h"
#include "raster_analysis.h"
#include "raster_region.h"
#include "raster_utils.h"
#include "stockpile.h"
#include "volume.h"
#include "raster_profile.h"

namespace
{
    using namespace ddb;
    using dq06::kNodata;

    // -9999.9 is not a float32: a Float64 raster declares it, and the analyses read the pixels as
    // float32, where they hold the nearest float, 4e-4 away. (GDAL rounds the nodata value of a
    // Float32 GeoTIFF itself, so only wider types show it.)
    constexpr double kUnrepresentable = -9999.9;
    const double kNaN = std::numeric_limits<double>::quiet_NaN();

    class Dq06Nodata : public ::testing::Test
    {
    protected:
        void SetUp() override { DDBRegisterProcess(false); }
    };

    TEST_F(Dq06Nodata, theRuleOnItsOwn)
    {
        const BandNodata none;
        EXPECT_FALSE(isNodata(0.0f, none));
        EXPECT_FALSE(isNodata(std::nanf(""), none));

        const BandNodata value{true, -9999.0};
        EXPECT_TRUE(isNodata(-9999.0f, value));
        EXPECT_TRUE(isNodata(-9999.0, value));
        EXPECT_FALSE(isNodata(-9999.5f, value));
        EXPECT_FALSE(isNodata(std::nanf(""), value));

        // A value that float32 cannot hold matches the float32 sample, not the double one
        const BandNodata odd{true, kUnrepresentable};
        EXPECT_TRUE(isNodata(static_cast<float>(kUnrepresentable), odd));
        EXPECT_FALSE(isNodata(static_cast<double>(static_cast<float>(kUnrepresentable)), odd))
            << "doubles are compared exactly: a float widened to double is another number";

        const BandNodata nan{true, kNaN};
        EXPECT_TRUE(isNodata(std::nanf(""), nan));
        EXPECT_TRUE(isNodata(kNaN, nan));
        EXPECT_FALSE(isNodata(0.0f, nan));
        EXPECT_FALSE(isNodata(-9999.0f, nan));

        // infinities are not nodata unless declared
        EXPECT_FALSE(isNodata(std::numeric_limits<float>::infinity(), value));
        EXPECT_TRUE(isNodata(std::numeric_limits<float>::infinity(), BandNodata{true, std::numeric_limits<double>::infinity()}));
    }

    // ---- the analyses ignore a nodata value that float32 cannot hold ---------------------------

    // The golden outputs were taken on the same terrain with the nodata value -9999
    TEST_F(Dq06Nodata, volumeIgnoresNodataPixelsOfAFloatTheNodataValueCannotHold)
    {
        TestArea ta(TEST_NAME, true);
        const auto &spec = dq06::demSpecs()[0];
        const fs::path dem = dq06::makeDem(ta.getPath("odd.tif"), spec, kUnrepresentable, static_cast<float>(kUnrepresentable), GDT_Float64);
        const json holey = dq06::polygon(spec, {{6, 44}, {20, 44}, {20, 64}, {6, 64}});
        dq06::expectGolden("dem_wgs84_volume_nodata", dq06::capture([&] {
                               return calculateVolumeJson(dem.string(), holey.dump(), "lowest_perimeter", 0.0);
                           }));
    }

    TEST_F(Dq06Nodata, profileShowsNullOverNodataPixelsOfAFloatTheNodataValueCannotHold)
    {
        TestArea ta(TEST_NAME, true);
        const auto &spec = dq06::demSpecs()[0];
        const fs::path dem = dq06::makeDem(ta.getPath("odd.tif"), spec, kUnrepresentable, static_cast<float>(kUnrepresentable), GDT_Float64);
        const json overNodata = dq06::lineString(spec, {{12.5, 45.5}, {12.5, 60.5}});
        dq06::expectGolden("dem_wgs84_profile_nodata", dq06::capture([&] {
                               return getRasterProfileJson(dem.string(), overNodata.dump(), 31);
                           }));
    }

    TEST_F(Dq06Nodata, stockpilesIgnoreNodataPixelsOfAFloatTheNodataValueCannotHold)
    {
        TestArea ta(TEST_NAME, true);
        const auto &spec = dq06::demSpecs()[0];
        const fs::path dem = dq06::makeDem(ta.getPath("odd.tif"), spec, kUnrepresentable, static_cast<float>(kUnrepresentable), GDT_Float64);
        const auto [lon, lat] = dq06::lonLatOfPixel(spec, 35.5, 35.5);
        dq06::expectGolden("dem_wgs84_stockpile", dq06::capture([&] {
                               return detectStockpileJson(dem.string(), lat, lon, 300.0, 0.5f);
                           }));
        dq06::expectGolden("dem_wgs84_stockpile_all", dq06::capture([&] {
                               return detectAllStockpilesJson(dem.string(), 0.5f, 0.0, 10);
                           }));
    }

    // The statistics of a formula, whose nodata masking is premaskNodata
    TEST_F(Dq06Nodata, formulaStatisticsMaskNodataPixelsOfAFloatTheNodataValueCannotHold)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path reference = dq06::makeMultispectral(ta.getPath("reference.tif"), kNodata, kNodata);
        const fs::path odd = dq06::makeMultispectral(ta.getPath("odd.tif"), kUnrepresentable, kUnrepresentable, GDT_Float64);

        auto statistics = [](const fs::path &path) {
            char *buffer = nullptr;
            EXPECT_EQ(DDBGetRasterMetadata(path.string().c_str(), "NDVI", "RGBReN", &buffer), DDBERR_NONE);
            json j = json::parse(buffer);
            DDBFree(buffer);
            return j["statistics"];
        };
        EXPECT_EQ(statistics(odd), statistics(reference));
    }

    // ---- a NaN nodata value ------------------------------------------------------------------

    json renderedPixels(const fs::path &raster)
    {
        uint8_t *buffer = nullptr;
        int size = 0;
        const double bbox[4] = {dq06::kOriginX, dq06::kOriginY - dq06::kSize * dq06::kPixel,
                                dq06::kOriginX + dq06::kSize * dq06::kPixel, dq06::kOriginY};
        renderRasterIndex(testutils::regionRequest(raster.string(), bbox, "EPSG:3857", 64, 64, "image/png"), "EVI", &buffer, &size);
        const auto img = testutils::decodeImage(testutils::takeBuffer(buffer, size));
        size_t transparent = 0;
        for (const uint8_t a : img.alpha())
            transparent += (a == 0);
        std::string all;
        for (const auto &plane : img.bands)
            all += dq06::fnv1a(plane.data(), plane.size());
        return {{"transparent", transparent}, {"pixels", dq06::fnv1a(all.data(), all.size())}};
    }

    TEST_F(Dq06Nodata, aNaNNodataValueOnlyMasksTheNaNPixelsOfTheIndexRender)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path reference = dq06::makeMultispectral(ta.getPath("reference.tif"), kNodata, kNodata);
        const fs::path nan = dq06::makeMultispectral(ta.getPath("nan.tif"), kNaN, kNaN);
        const json expected = renderedPixels(reference);
        ASSERT_GT(expected["transparent"].get<int>(), 0);
        ASSERT_LT(expected["transparent"].get<int>(), 64 * 64);
        // Before DQ-06 every pixel of the NaN raster was transparent
        EXPECT_EQ(renderedPixels(nan), expected);
    }

    // The mean of a DEM that align wrote, which must be the reference's: align measures the
    // elevation offset between the two and subtracts it
    double meanOf(const fs::path &path)
    {
        std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset *>(GDALOpen(path.string().c_str(), GA_ReadOnly)));
        double min = 0, max = 0, mean = 0, sd = 0;
        ds->GetRasterBand(1)->ComputeStatistics(FALSE, &min, &max, &mean, &sd, nullptr, nullptr);
        return mean;
    }

    TEST_F(Dq06Nodata, alignAppliesTheElevationOffsetOfADemWithANaNNodataValue)
    {
        TestArea ta(TEST_NAME, true);
        const auto make = [&](const std::string &name, double declared, double offset) {
            const fs::path p = ta.getPath(name + ".tif");
            dq06::makeRaster(p, 1, GDT_Float32, false, std::nullopt, 256);
            std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset *>(GDALOpen(p.string().c_str(), GA_Update)));
            std::vector<float> plane(256 * 256);
            GDALRasterBand *band = ds->GetRasterBand(1);
            dq06::checkIo(band->RasterIO(GF_Read, 0, 0, 256, 256, plane.data(), 256, 256, GDT_Float32, 0, 0));
            for (int y = 0; y < 256; y++)
                for (int x = 0; x < 256; x++)
                    plane[y * 256 + x] = (x < 10 && y < 10) ? static_cast<float>(declared) : plane[y * 256 + x] + static_cast<float>(offset);
            dq06::checkIo(band->RasterIO(GF_Write, 0, 0, 256, 256, plane.data(), 256, 256, GDT_Float32, 0, 0));
            band->SetNoDataValue(declared);
            return p;
        };
        const fs::path reference = make("reference", kNaN, 0.0);
        const fs::path source = make("source", kNaN, 5.0);
        const fs::path output = ta.getPath("aligned.tif");
        AlignOptions options;
        options.mode = AlignMode::Translation;
        const AlignResult r = alignRaster(source.string(), reference.string(), output.string(), options);
        EXPECT_NEAR(r.shiftZ, -5.0, 0.1);
        // The offset is applied to the data, not only reported: before DQ-06 the output stayed 5 m up
        EXPECT_NEAR(meanOf(output), meanOf(reference), 0.5);
    }
}
