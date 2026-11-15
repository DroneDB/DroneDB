/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"

#include "cog.h"
#include "mask.h"
#include "ddb.h"
#include "exceptions.h"
#include "gdal_inc.h"
#include "raster_test_utils.h"
#include "user_open_child.h"
#include <cmath>
#include <functional>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using namespace ddb;

// ---------------------------------------------------------------------------
// Helper: create a small synthetic RGB GeoTIFF with a black border collar.
// The centre pixels are set to (128, 64, 32); the outer border ring is black
// (0, 0, 0). EPSG:32632 (UTM Zone 32N) is used so buildCog can reproject.
// ---------------------------------------------------------------------------
static fs::path createRgbWithBlackBorder(const fs::path &path,
                                         int w = 64, int h = 64,
                                         int borderPx = 8)
{
    GDALDriverH drv = GDALGetDriverByName("GTiff");
    if (!drv) throw std::runtime_error("GTiff driver not available");

    GDALDatasetH hDs = GDALCreate(drv, path.string().c_str(),
                                   w, h, 3, GDT_Byte, nullptr);
    if (!hDs) throw std::runtime_error("Cannot create test raster");

    // Minimal georeference in EPSG:32632 so the COG builder can reproject.
    double gt[6] = {500000.0, 1.0, 0.0, 4500000.0, 0.0, -1.0};
    GDALSetGeoTransform(hDs, gt);

    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 32632);
    char *wkt = nullptr;
    OSRExportToWkt(srs, &wkt);
    GDALSetProjection(hDs, wkt);
    CPLFree(wkt);
    OSRDestroySpatialReference(srs);

    // Fill each band: border pixels = 0 (black collar), interior = colour value.
    const uint8_t colours[3] = {128, 64, 32};
    std::vector<uint8_t> row(static_cast<size_t>(w));

    for (int b = 1; b <= 3; b++) {
        GDALRasterBandH hBand = GDALGetRasterBand(hDs, b);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                bool isBorder = (x < borderPx || x >= w - borderPx ||
                                 y < borderPx || y >= h - borderPx);
                row[static_cast<size_t>(x)] = isBorder ? 0 : colours[b - 1];
            }
            GDALRasterIO(hBand, GF_Write, 0, y, w, 1,
                         row.data(), w, 1, GDT_Byte, 0, 0);
        }
    }

    GDALFlushCache(hDs);
    GDALClose(hDs);
    return path;
}

// ---------------------------------------------------------------------------
// Helper: create a 4-band Byte GeoTIFF where band 4 has NO alpha colour
// interpretation (simulating an RGB+NIR multispectral raster).
// ---------------------------------------------------------------------------
static fs::path createFourBandNonAlpha(const fs::path &path)
{
    GDALDriverH drv = GDALGetDriverByName("GTiff");
    if (!drv) throw std::runtime_error("GTiff driver not available");

    GDALDatasetH hDs = GDALCreate(drv, path.string().c_str(),
                                   32, 32, 4, GDT_Byte, nullptr);
    if (!hDs) throw std::runtime_error("Cannot create test raster");

    // Bands 1-4: set to GCI_GrayIndex (or leave default = undefined).
    // Deliberately do NOT set band 4 to GCI_AlphaBand.
    for (int b = 1; b <= 4; b++) {
        GDALRasterBandH hBand = GDALGetRasterBand(hDs, b);
        GDALSetRasterColorInterpretation(hBand, GCI_GrayIndex);
    }

    double gt[6] = {500000.0, 1.0, 0.0, 4500000.0, 0.0, -1.0};
    GDALSetGeoTransform(hDs, gt);

    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 32632);
    char *wkt = nullptr;
    OSRExportToWkt(srs, &wkt);
    GDALSetProjection(hDs, wkt);
    CPLFree(wkt);
    OSRDestroySpatialReference(srs);

    GDALFlushCache(hDs);
    GDALClose(hDs);
    return path;
}

// ===========================================================================
// maskBorders tests
// ===========================================================================

TEST(mask, rejectsFourBandNonAlpha) {
    TestArea ta(TEST_NAME);
    fs::path input = createFourBandNonAlpha(ta.getPath("4band_nir.tif"));
    fs::path output = ta.getPath("4band_nir_masked.tif");

    EXPECT_THROW(
        ddb::maskBorders(input.string(), output.string()),
        InvalidArgsException
    ) << "4-band input without an alpha band should be rejected";
}

TEST(mask, acceptsFourBandWithAlpha) {
    TestArea ta(TEST_NAME);

    // Build a 4-band RGBA raster (band 4 = GCI_AlphaBand).
    GDALDriverH drv = GDALGetDriverByName("GTiff");
    ASSERT_NE(drv, nullptr);
    fs::path input = ta.getPath("rgba_input.tif");
    GDALDatasetH hDs = GDALCreate(drv, input.string().c_str(),
                                   64, 64, 4, GDT_Byte, nullptr);
    ASSERT_NE(hDs, nullptr);

    double gt[6] = {500000.0, 1.0, 0.0, 4500000.0, 0.0, -1.0};
    GDALSetGeoTransform(hDs, gt);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 32632);
    char *wkt = nullptr;
    OSRExportToWkt(srs, &wkt);
    GDALSetProjection(hDs, wkt);
    CPLFree(wkt);
    OSRDestroySpatialReference(srs);

    // Mark band 4 as alpha and fill with full-opacity (255).
    GDALRasterBandH hBand4 = GDALGetRasterBand(hDs, 4);
    GDALSetRasterColorInterpretation(hBand4, GCI_AlphaBand);
    std::vector<uint8_t> row(64, 255);
    for (int y = 0; y < 64; y++)
        GDALRasterIO(hBand4, GF_Write, 0, y, 64, 1, row.data(), 64, 1, GDT_Byte, 0, 0);

    GDALFlushCache(hDs);
    GDALClose(hDs);

    fs::path output = ta.getPath("rgba_masked.tif");
    EXPECT_NO_THROW(ddb::maskBorders(input.string(), output.string()))
        << "4-band RGBA input should be accepted by maskBorders";
    EXPECT_TRUE(fs::exists(output));
}

// ===========================================================================
// COG regression: PER_DATASET mask must be preserved after buildCog
// ===========================================================================

TEST(mask, cogPreservesPerDatasetMask) {
    TestArea ta(TEST_NAME);

    // 1. Create a small RGB raster with a black border collar.
    fs::path rgbInput = createRgbWithBlackBorder(ta.getPath("rgb_input.tif"));

    // 2. Run maskBorders -> produces an internal PER_DATASET mask (dataset mask,
    //    not an alpha band) via nearblack -setmask.
    fs::path maskedGtiff = ta.getPath("rgb_masked.tif");
    ASSERT_NO_THROW(ddb::maskBorders(rgbInput.string(), maskedGtiff.string()));
    ASSERT_TRUE(fs::exists(maskedGtiff));

    // Sanity: the intermediate GeoTIFF must carry a stand-alone PER_DATASET mask.
    {
        GDALDatasetH hDs = GDALOpen(maskedGtiff.string().c_str(), GA_ReadOnly);
        ASSERT_NE(hDs, nullptr);
        GDALRasterBandH hBand1 = GDALGetRasterBand(hDs, 1);
        ASSERT_NE(hBand1, nullptr);
        const int maskFlags = GDALGetMaskFlags(hBand1);
        GDALClose(hDs);

        const bool hasPerDatasetMask =
            (maskFlags & GMF_PER_DATASET) != 0 &&
            (maskFlags & GMF_ALPHA) == 0 &&
            (maskFlags & GMF_NODATA) == 0;
        EXPECT_TRUE(hasPerDatasetMask)
            << "maskBorders output should carry a PER_DATASET (non-alpha) mask; "
               "got mask flags = " << maskFlags;
    }

    // 3. Build COG from the masked intermediate.
    fs::path cogOutput = ta.getPath("rgb_cog.tif");
    ASSERT_NO_THROW(ddb::buildCog(maskedGtiff.string(), cogOutput.string()));
    ASSERT_TRUE(fs::exists(cogOutput));

    // 4. The COG must expose transparency, as either:
    //    a) An explicit alpha band (GMF_ALPHA set), or
    //    b) A stand-alone PER_DATASET dataset mask (GMF_PER_DATASET set,
    //       GMF_ALL_VALID NOT set, GMF_NODATA NOT set).
    //    A COG with JPEG tiles keeps the transparency in an internal mask
    //    IFD, which reads back as b); checking only for GMF_ALPHA would
    //    miss it.
    {
        GDALDatasetH hCog = GDALOpen(cogOutput.string().c_str(), GA_ReadOnly);
        ASSERT_NE(hCog, nullptr);

        GDALRasterBandH hBand1 = GDALGetRasterBand(hCog, 1);
        ASSERT_NE(hBand1, nullptr);
        const int maskFlags = GDALGetMaskFlags(hBand1);
        const bool hasTransparency =
            (maskFlags & GMF_ALPHA) != 0 ||
            ((maskFlags & GMF_PER_DATASET) != 0 &&
             (maskFlags & GMF_ALL_VALID) == 0 &&
             (maskFlags & GMF_NODATA) == 0);
        GDALClose(hCog);

        EXPECT_TRUE(hasTransparency)
            << "COG built from a PER_DATASET-masked raster must preserve "
               "transparency; got mask flags = " << maskFlags;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Rasters of one band of more than 8 bits (DEMs, thermal rasters): nearblack writes 8 bits only
// ---------------------------------------------------------------------------
namespace masksamples {

using testutils::GeoRasterSpec;

// A collar near black (0 to 10) with a concave notch and short runs of bright pixels (which
// nearblack's -nb 2 passes over), around data from 40 up, with a black hole inside that no border
// reaches
double collarSample(int x, int y) {
    const bool notch = x >= 30 && x < 40 && y < 50;
    const bool collar = x < 9 || y < 7 || x >= 87 || y >= 90 || notch;
    if (collar)
        return (x * 3 + y * 5) % 11 == 0 && (x + y) % 2 == 0 ? 120 : (x + 2 * y) % 11;
    if (x >= 60 && x < 66 && y >= 60 && y < 66)
        return 0;  // the hole
    return 40 + (x * 7 + y * 13) % 200;
}

// The internal mask of band 1 of `file`
std::vector<uint8_t> maskOf(const fs::path &file) {
    GDALDatasetH ds = GDALOpen(file.string().c_str(), GA_ReadOnly);
    EXPECT_NE(ds, nullptr) << file;
    GDALRasterBandH band = GDALGetRasterBand(ds, 1);
    EXPECT_EQ(GDALGetMaskFlags(band), GMF_PER_DATASET);
    const int w = GDALGetRasterXSize(ds), h = GDALGetRasterYSize(ds);
    std::vector<uint8_t> mask(static_cast<size_t>(w) * h);
    EXPECT_EQ(GDALRasterIO(GDALGetMaskBand(band), GF_Read, 0, 0, w, h, mask.data(), w, h, GDT_Byte, 0, 0), CE_None);
    GDALClose(ds);
    return mask;
}

std::vector<double> samplesOf(const fs::path &file, GDALDataType *type = nullptr) {
    GDALDatasetH ds = GDALOpen(file.string().c_str(), GA_ReadOnly);
    EXPECT_NE(ds, nullptr) << file;
    GDALRasterBandH band = GDALGetRasterBand(ds, 1);
    if (type != nullptr)
        *type = GDALGetRasterDataType(band);
    const int w = GDALGetRasterXSize(ds), h = GDALGetRasterYSize(ds);
    std::vector<double> v(static_cast<size_t>(w) * h);
    EXPECT_EQ(GDALRasterIO(band, GF_Read, 0, 0, w, h, v.data(), w, h, GDT_Float64, 0, 0), CE_None);
    GDALClose(ds);
    return v;
}

fs::path oneBand(const fs::path &path, GDALDataType type, const std::function<double(int, int)> &value) {
    GeoRasterSpec spec;
    spec.width = spec.height = 96;
    spec.bands = 1;
    spec.type = type;
    spec.value = [value](int, int x, int y) { return value(x, y); };
    return testutils::createGeoRaster(path, spec);
}

raster::MaskParams params(bool white = false, const std::string &color = "") {
    ddb::raster::MaskParams p;
    p.white = white;
    p.color = color;
    return p;
}

}  // namespace masksamples

// The mask of a raster of 16 bits is the mask nearblack makes of the same samples in 8 bits: black,
// white and a colour, the notch, the bright runs and the hole no border reaches included
USER_OPEN_TEST(mask, aRasterOfMoreThan8BitsGetsTheMaskNearblackMakesOfItsSamples) {
    using namespace masksamples;
    TestArea ta(TEST_NAME, true);
    const struct {
        const char *name;
        std::function<double(int, int)> value;
        ddb::raster::MaskParams params;
    } cases[] = {{"black", collarSample, params()},
                 {"white", [](int x, int y) { return 255 - collarSample(x, y); }, params(true)},
                 {"color", [](int x, int y) { const double v = collarSample(x, y); return v < 11 ? 100 + v : v; },
                  params(false, "100")},
                 {"color255", [](int x, int y) { return 255 - collarSample(x, y); }, params(false, "255")}};
    for (const auto &c : cases) {
        SCOPED_TRACE(c.name);
        const auto bytes = oneBand(ta.getPath(std::string(c.name) + "8.tif"), GDT_Byte, c.value);
        const auto wide = oneBand(ta.getPath(std::string(c.name) + "16.tif"), GDT_UInt16, c.value);
        ddb::raster::maskRasterFile(bytes, ta.getPath(std::string(c.name) + "8-masked.tif"), c.params, {});
        ddb::raster::maskRasterFile(wide, ta.getPath(std::string(c.name) + "16-masked.tif"), c.params, {});
        const auto expected = maskOf(ta.getPath(std::string(c.name) + "8-masked.tif"));
        EXPECT_EQ(maskOf(ta.getPath(std::string(c.name) + "16-masked.tif")), expected);
        // The test means something: the collar is masked, the hole is not
        EXPECT_EQ(expected[1], 0);
        EXPECT_EQ(expected[62 * 96 + 62], 255);
    }
}

// A DEM keeps its type and every sample (nearblack cut 1234 to 255, and refused Float32); the
// samples that are NoData or NaN are masked as well
USER_OPEN_TEST(mask, aDemKeepsItsTypeAndItsSamples) {
    using namespace masksamples;
    TestArea ta(TEST_NAME, true);
    const auto dem = [](int x, int y) { const double v = collarSample(x, y); return v < 40 ? v : 1000 + v * 3.25; };
    for (const GDALDataType type : {GDT_Int16, GDT_UInt16, GDT_Float32, GDT_Float64}) {
        SCOPED_TRACE(GDALGetDataTypeName(type));
        const std::string name = GDALGetDataTypeName(type);
        const auto in = oneBand(ta.getPath(name + ".tif"), type, dem);
        const bool floating = type == GDT_Float32 || type == GDT_Float64;
        {
            GDALDatasetH ds = GDALOpen(in.string().c_str(), GA_Update);
            ASSERT_NE(ds, nullptr);
            GDALRasterBandH band = GDALGetRasterBand(ds, 1);
            ASSERT_EQ(GDALSetRasterNoDataValue(band, 7), CE_None);
            double v = 7;
            ASSERT_EQ(GDALRasterIO(band, GF_Write, 50, 20, 1, 1, &v, 1, 1, GDT_Float64, 0, 0), CE_None);
            v = std::nan("");
            if (floating) {
                ASSERT_EQ(GDALRasterIO(band, GF_Write, 51, 20, 1, 1, &v, 1, 1, GDT_Float64, 0, 0), CE_None);
            }
            GDALClose(ds);
        }
        const auto out = ta.getPath(name + "-masked.tif");
        ddb::raster::maskRasterFile(in, out, params(), {});

        GDALDataType outType = GDT_Unknown;
        const auto before = samplesOf(in);
        const auto after = samplesOf(out, &outType);
        EXPECT_EQ(outType, type);
        ASSERT_EQ(after.size(), before.size());
        for (size_t i = 0; i < after.size(); i++) {
            if (!(std::isnan(before[i]) && std::isnan(after[i]))) {
                ASSERT_EQ(after[i], before[i]) << i;
            }
        }
        const auto mask = maskOf(out);
        EXPECT_EQ(mask[1], 0) << "the collar";
        EXPECT_EQ(mask[45 * 96 + 45], 255) << "the data";
        EXPECT_EQ(mask[20 * 96 + 50], 0) << "NoData";
        if (floating) {
            EXPECT_EQ(mask[20 * 96 + 51], 0) << "NaN";
        }
    }
}

// The border colour of a raster of one band is one number
USER_OPEN_TEST(mask, theColourOfARasterOfOneBandIsOneNumber) {
    using namespace masksamples;
    TestArea ta(TEST_NAME, true);
    const auto in = oneBand(ta.getPath("dem.tif"), GDT_Int16, collarSample);
    EXPECT_THROW(ddb::raster::maskRasterFile(in, ta.getPath("a.tif"), params(false, "1,2"), {}), InvalidArgsException);
    EXPECT_THROW(ddb::raster::maskRasterFile(in, ta.getPath("b.tif"), params(false, "dark"), {}), InvalidArgsException);
}
