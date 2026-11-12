/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The opaque value of an alpha band (alphaOpaqueValue) comes from the full-resolution pixels. A COG
// written by GDAL's COG driver with its defaults has cubic overviews, which ring above the opaque
// value at the edge of the data (266 where the band holds 255): the approximate maximum, read from
// those overviews, made a UInt16 or Float32 alpha band holding 255 opaque at 511 and every opaque
// pixel half transparent.

#include <cmath>
#include <string>
#include <vector>

#include "ddb.h"
#include "fs.h"
#include "gdal_inc.h"
#include "gdal_utils.h"
#include "gtest/gtest.h"
#include "raster_test_utils.h"
#include "raster_utils.h"
#include "test.h"
#include "testarea.h"

namespace {

using namespace ddb;

// A COG of three data bands and an alpha band of `type`: `opaque` inside a disc, 0 outside it
// (a sharp edge for the cubic overviews to ring on), written by GDAL's COG driver with its defaults.
fs::path cogWithAlpha(TestArea& ta, const std::string& name, GDALDataType type, double opaque, int size = 1500,
                      double radiusFraction = 0.45, bool corridor = false) {
    testutils::GeoRasterSpec spec;
    spec.width = spec.height = size;
    spec.bands = 4;
    spec.type = type;
    spec.alphaBand = true;
    const double c = size / 2.0, r = size * radiusFraction;
    spec.alphaValue = [=](int x, int y) {
        // A corridor: a diagonal strip of 2 % of the width that misses the coarse sample grid
        if (corridor) return std::abs((x - 0.37 * size) - (y - 0.07 * size)) < 0.01 * size ? opaque : 0.0;
        const double dx = x + 0.5 - c, dy = y + 0.5 - c;
        return dx * dx + dy * dy <= r * r ? opaque : 0.0;
    };
    spec.value = [](int b, int x, int y) { return static_cast<double>((x * 7 + y * 3 + b * 50) % 200); };
    const fs::path src = testutils::createGeoRaster(ta.getPath(name + ".src.tif"), spec);

    GDALDatasetH in = GDALOpen(src.string().c_str(), GA_ReadOnly);
    EXPECT_NE(in, nullptr);
    char* argv[] = {const_cast<char*>("-of"), const_cast<char*>("COG"), nullptr};
    GDALTranslateOptions* opts = GDALTranslateOptionsNew(argv, nullptr);
    const fs::path cog = ta.getPath(name + ".cog.tif");
    GDALDatasetH out = GDALTranslate(cog.string().c_str(), in, opts, nullptr);
    EXPECT_NE(out, nullptr);
    GDALTranslateOptionsFree(opts);
    GDALClose(out);
    GDALClose(in);
    return cog;
}

double opaqueOf(const fs::path& path) {
    GDALDatasetH ds = GDALOpen(path.string().c_str(), GA_ReadOnly);
    EXPECT_NE(ds, nullptr);
    if (ds == nullptr) return -1;
    const double v = alphaOpaqueValue(GDALGetRasterBand(ds, GDALGetRasterCount(ds)));
    GDALClose(ds);
    return v;
}

double overviewMaximum(const fs::path& path) {
    GDALDatasetH ds = GDALOpen(path.string().c_str(), GA_ReadOnly);
    GDALRasterBandH alpha = GDALGetRasterBand(ds, GDALGetRasterCount(ds));
    double minMax[2] = {0, 0};
    GDALComputeRasterMinMax(alpha, TRUE, minMax);
    GDALClose(ds);
    return minMax[1];
}

class AlphaOpaque : public ::testing::Test {
protected:
    void SetUp() override { DDBRegisterProcess(false); }
};

// The overviews of these COGs ring above the opaque value (the case that broke), and the opaque
// value is still the band's own: 255 in a UInt16 or Float32 band, 1 in a Float32 band of 0..1
TEST_F(AlphaOpaque, isTheFullResolutionValueOfACogWithCubicOverviews) {
    TestArea ta(TEST_NAME, true);
    struct Case {
        const char* name;
        GDALDataType type;
        double opaque;
        double expected;
    };
    for (const Case& c : {Case{"u16-255", GDT_UInt16, 255, 255}, Case{"f32-255", GDT_Float32, 255, 255},
                          Case{"f32-1", GDT_Float32, 1, 1}, Case{"i16-255", GDT_Int16, 255, 255},
                          Case{"byte-255", GDT_Byte, 255, 255}, Case{"u16-65535", GDT_UInt16, 65535, 65535},
                          Case{"u32-255", GDT_UInt32, 255, 255}, Case{"f64-255", GDT_Float64, 255, 255}}) {
        const fs::path cog = cogWithAlpha(ta, c.name, c.type, c.opaque);
        // The floating-point and wider integer bands ring above the opaque value in their
        // overviews (the case that broke); Byte saturates at 255
        if (c.type == GDT_Float32 || c.type == GDT_Float64 || c.type == GDT_Int16 || c.type == GDT_UInt32) {
            EXPECT_GT(overviewMaximum(cog), c.opaque) << c.name << ": the overviews do not ring here";
        }
        EXPECT_EQ(opaqueOf(cog), c.expected) << c.name;
    }
}

// Corner cases: data in a small part of the raster only (the sample may miss it: it is found
// anyway), a raster smaller than a block, sizes that are not a multiple of the block, a band with
// no opaque pixel at all (the maximum of its type, as before)
TEST_F(AlphaOpaque, isFoundInSparseTinyAndOddRasters) {
    TestArea ta(TEST_NAME, true);
    EXPECT_EQ(opaqueOf(cogWithAlpha(ta, "sparse", GDT_Float32, 255, 3000, 0.02)), 255);
    EXPECT_EQ(opaqueOf(cogWithAlpha(ta, "corridor", GDT_Float32, 255, 3000, 0, true)), 255);
    EXPECT_EQ(opaqueOf(cogWithAlpha(ta, "tiny", GDT_UInt16, 255, 7, 0.4)), 255);
    EXPECT_EQ(opaqueOf(cogWithAlpha(ta, "odd", GDT_Float32, 255, 1031, 0.45)), 255);
    EXPECT_EQ(opaqueOf(cogWithAlpha(ta, "none-u16", GDT_UInt16, 255, 300, 0.0)), 65535);
    EXPECT_EQ(opaqueOf(cogWithAlpha(ta, "none-f32", GDT_Float32, 255, 300, 0.0)), 1);
}

}  // namespace
