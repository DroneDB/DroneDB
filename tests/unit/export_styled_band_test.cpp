/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The export of one band with a colormap and no formula (FR-RAS-26, D-023): the RGBA picture that the colormap makes
// of the band, as the picture of a formula is; the other exports (a band without a colormap, several bands with
// one) are unchanged. The expected pixels are the colours of the colormap table at the index the value gives, worked
// out here by hand, and the picture of the formula export over the same values (CELSIUS over a band in degrees
// Celsius is the identity), which is drawn by the code the map tiles of a formula use.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ddb.h"
#include "fs.h"
#include "gdal_inc.h"
#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"
#include "vegetation.h"

namespace {

constexpr int kWidth = 64;
constexpr int kHeight = 48;
constexpr float kNodata = -9999.0f;

// The value of the pixel (x, y) of the DEM: 0 at the top left to 677 at the bottom right, no data in a 2 x 2 block
float demValue(int x, int y) {
    if (x >= 12 && x <= 13 && y >= 6 && y <= 7) return kNodata;
    return static_cast<float>(10 * x + y);
}

void setWgs84(GDALDatasetH ds) {
    const double gt[6] = {10.0, 0.001, 0.0, 45.0, 0.0, -0.001};
    GDALSetGeoTransform(ds, const_cast<double*>(gt));
    OGRSpatialReference srs;
    srs.importFromEPSG(4326);
    char* wkt = nullptr;
    srs.exportToWkt(&wkt);
    GDALSetProjection(ds, wkt);
    CPLFree(wkt);
}

// A DEM of one Float32 band with a NoData value
fs::path makeDem(const fs::path& path) {
    GDALDatasetH ds = GDALCreate(GDALGetDriverByName("GTiff"), path.string().c_str(), kWidth, kHeight, 1, GDT_Float32, nullptr);
    if (ds == nullptr) throw std::runtime_error("Cannot create the DEM");
    setWgs84(ds);
    GDALRasterBandH band = GDALGetRasterBand(ds, 1);
    GDALSetRasterNoDataValue(band, kNodata);
    std::vector<float> row(kWidth);
    for (int y = 0; y < kHeight; y++) {
        for (int x = 0; x < kWidth; x++) row[x] = demValue(x, y);
        if (GDALRasterIO(band, GF_Write, 0, y, kWidth, 1, row.data(), kWidth, 1, GDT_Float32, 0, 0) != CE_None)
            throw std::runtime_error("Cannot write the DEM");
    }
    GDALClose(ds);
    return path;
}

// A raster of three Byte bands
fs::path makeRgb(const fs::path& path) {
    GDALDatasetH ds = GDALCreate(GDALGetDriverByName("GTiff"), path.string().c_str(), kWidth, kHeight, 3, GDT_Byte, nullptr);
    if (ds == nullptr) throw std::runtime_error("Cannot create the RGB raster");
    setWgs84(ds);
    std::vector<uint8_t> row(kWidth);
    for (int b = 1; b <= 3; b++) {
        for (int y = 0; y < kHeight; y++) {
            for (int x = 0; x < kWidth; x++) row[x] = static_cast<uint8_t>((x * 3 + y * 2 + b * 40) % 256);
            if (GDALRasterIO(GDALGetRasterBand(ds, b), GF_Write, 0, y, kWidth, 1, row.data(), kWidth, 1, GDT_Byte, 0, 0) !=
                CE_None)
                throw std::runtime_error("Cannot write the RGB raster");
        }
    }
    GDALClose(ds);
    return path;
}

// What an export wrote: the size, the type of the bands and the pixels of each band
struct Picture {
    int width = 0, height = 0, bands = 0;
    GDALDataType type = GDT_Unknown;
    double gt[6] = {};
    std::vector<std::vector<uint8_t>> pixels;  // per band

    uint8_t at(int band, int x, int y) const { return pixels[band][static_cast<size_t>(y) * width + x]; }
};

Picture readPicture(const fs::path& path) {
    Picture p;
    GDALDatasetH ds = GDALOpen(path.string().c_str(), GA_ReadOnly);
    EXPECT_NE(ds, nullptr) << path;
    if (ds == nullptr) return p;
    p.width = GDALGetRasterXSize(ds);
    p.height = GDALGetRasterYSize(ds);
    p.bands = GDALGetRasterCount(ds);
    p.type = GDALGetRasterDataType(GDALGetRasterBand(ds, 1));
    GDALGetGeoTransform(ds, p.gt);
    for (int b = 1; b <= p.bands; b++) {
        std::vector<uint8_t> data(static_cast<size_t>(p.width) * p.height);
        EXPECT_EQ(GDALRasterIO(GDALGetRasterBand(ds, b), GF_Read, 0, 0, p.width, p.height, data.data(), p.width,
                               p.height, GDT_Byte, 0, 0),
                  CE_None);
        p.pixels.push_back(std::move(data));
    }
    GDALClose(ds);
    return p;
}

DDBRasterOptions windowOptions() {
    // The centres of the pixels are at 10 + (x + 0.5) * 0.001 and 45 - (y + 0.5) * 0.001: this box holds the columns
    // 10..29 and the rows 5..24 (20 x 20 pixels, from the corner (10.010, 44.995))
    DDBRasterOptions options = {};
    options.structSize = sizeof(options);
    options.flags = DDB_RASTER_OPT_BBOX;
    const double bbox[4] = {10.0102, 44.9752, 10.0298, 44.9948};
    std::copy(bbox, bbox + 4, options.bbox);
    return options;
}

DDBErr exportAs(const fs::path& in, const fs::path& out, const char* bands, const char* formula, const char* colormap,
                const char* rescale, const DDBRasterOptions* options = nullptr) {
    return DDBExportRaster3(in.string().c_str(), out.string().c_str(), nullptr, bands, formula, nullptr, colormap,
                            rescale, 0, nullptr, nullptr, options);
}

// The colour the table of the colormap gives to v over lo..hi, the rule of the colormap of the formulas
ddb::ColormapEntry colourOf(const std::string& id, float v, float lo, float hi) {
    const ddb::Colormap* colormap = ddb::VegetationEngine::instance().getColormap(id);
    const float normalized = std::max(0.0f, std::min(1.0f, (v - lo) / (hi - lo)));
    return colormap->entries[std::max(0, std::min(255, static_cast<int>(normalized * 255.0f)))];
}

class ExportStyledBand : public ::testing::Test {
protected:
    void SetUp() override { DDBRegisterProcess(false); }
};

TEST_F(ExportStyledBand, aDemWithAColormapAndARescaleIsTheRgbaPictureOfTheColormap) {
    TestArea ta(TEST_NAME);
    const fs::path dem = makeDem(ta.getPath("dem.tif"));
    const fs::path out = ta.getPath("styled.tif");

    ASSERT_EQ(exportAs(dem, out, nullptr, nullptr, "viridis", "0,500"), DDBERR_NONE) << DDBGetLastError();

    const Picture p = readPicture(out);
    EXPECT_EQ(p.bands, 4);
    EXPECT_EQ(p.type, GDT_Byte);
    EXPECT_EQ(p.width, kWidth);
    EXPECT_EQ(p.height, kHeight);
    // In the CRS and at the resolution of the file
    EXPECT_NEAR(p.gt[0], 10.0, 1e-12);
    EXPECT_NEAR(p.gt[1], 0.001, 1e-12);
    EXPECT_NEAR(p.gt[3], 45.0, 1e-12);

    for (const auto [x, y] : std::vector<std::pair<int, int>>{{0, 0}, {20, 10}, {30, 3}, {50, 40}, {63, 47}, {12, 5}}) {
        const ddb::ColormapEntry expected = colourOf("viridis", demValue(x, y), 0.0f, 500.0f);
        EXPECT_EQ(p.at(0, x, y), expected.r) << x << "," << y;
        EXPECT_EQ(p.at(1, x, y), expected.g) << x << "," << y;
        EXPECT_EQ(p.at(2, x, y), expected.b) << x << "," << y;
        EXPECT_EQ(p.at(3, x, y), 255) << x << "," << y;
    }
}

TEST_F(ExportStyledBand, theColoursAreThoseOfTheFormulaPictureOfTheSameValues) {
    TestArea ta(TEST_NAME);
    const fs::path dem = makeDem(ta.getPath("dem.tif"));

    // CELSIUS over a band in degrees Celsius keeps the values: the formula picture is drawn by the code that the
    // tiles of a formula use
    ASSERT_EQ(exportAs(dem, ta.getPath("formula.tif"), nullptr, "CELSIUS", "plasma", "50,400"), DDBERR_NONE)
        << DDBGetLastError();
    ASSERT_EQ(exportAs(dem, ta.getPath("band.tif"), nullptr, nullptr, "plasma", "50,400"), DDBERR_NONE)
        << DDBGetLastError();

    const Picture formula = readPicture(ta.getPath("formula.tif"));
    const Picture band = readPicture(ta.getPath("band.tif"));
    ASSERT_EQ(band.bands, 4);
    ASSERT_EQ(formula.bands, 4);
    for (int b = 0; b < 4; b++) EXPECT_EQ(band.pixels[b], formula.pixels[b]) << "band " << b + 1;
}

TEST_F(ExportStyledBand, withoutARescaleTheBandIsStretchedOverItsMinimumToItsMaximumAsTheTilesDo) {
    TestArea ta(TEST_NAME);
    const fs::path dem = makeDem(ta.getPath("dem.tif"));

    // The valid values go from 0 (pixel 0,0) to 677 (pixel 63,47); the NoData pixels are out of the statistics
    ASSERT_EQ(exportAs(dem, ta.getPath("auto.tif"), nullptr, nullptr, "inferno", nullptr), DDBERR_NONE)
        << DDBGetLastError();
    ASSERT_EQ(exportAs(dem, ta.getPath("explicit.tif"), nullptr, nullptr, "inferno", "0,677"), DDBERR_NONE)
        << DDBGetLastError();

    const Picture automatic = readPicture(ta.getPath("auto.tif"));
    const Picture explicitRange = readPicture(ta.getPath("explicit.tif"));
    ASSERT_EQ(automatic.bands, 4);
    for (int b = 0; b < 4; b++) EXPECT_EQ(automatic.pixels[b], explicitRange.pixels[b]) << "band " << b + 1;

    // The lowest value has the first colour and the highest the last
    const ddb::Colormap* inferno = ddb::VegetationEngine::instance().getColormap("inferno");
    EXPECT_EQ(automatic.at(0, 0, 0), inferno->entries[0].r);
    EXPECT_EQ(automatic.at(2, kWidth - 1, kHeight - 1), inferno->entries[255].b);
}

TEST_F(ExportStyledBand, thePixelsWithNoDataAreTransparent) {
    TestArea ta(TEST_NAME);
    const fs::path dem = makeDem(ta.getPath("dem.tif"));
    const fs::path out = ta.getPath("styled.tif");
    ASSERT_EQ(exportAs(dem, out, nullptr, nullptr, "viridis", "0,500"), DDBERR_NONE) << DDBGetLastError();

    const Picture p = readPicture(out);
    ASSERT_EQ(p.bands, 4);
    for (int y = 0; y < kHeight; y++)
        for (int x = 0; x < kWidth; x++) {
            const bool nodata = demValue(x, y) == kNodata;
            EXPECT_EQ(p.at(3, x, y), nodata ? 0 : 255) << x << "," << y;
        }
}

TEST_F(ExportStyledBand, aWindowIsTheCropOfTheWholeExportAtTheResolutionOfTheFile) {
    TestArea ta(TEST_NAME);
    const fs::path dem = makeDem(ta.getPath("dem.tif"));
    ASSERT_EQ(exportAs(dem, ta.getPath("whole.tif"), nullptr, nullptr, "viridis", "0,500"), DDBERR_NONE)
        << DDBGetLastError();
    const DDBRasterOptions options = windowOptions();
    ASSERT_EQ(exportAs(dem, ta.getPath("window.tif"), nullptr, nullptr, "viridis", "0,500", &options), DDBERR_NONE)
        << DDBGetLastError();

    const Picture whole = readPicture(ta.getPath("whole.tif"));
    const Picture window = readPicture(ta.getPath("window.tif"));
    ASSERT_EQ(window.bands, 4);
    EXPECT_EQ(window.width, 20);
    EXPECT_EQ(window.height, 20);
    EXPECT_NEAR(window.gt[0], 10.010, 1e-9);
    EXPECT_NEAR(window.gt[3], 44.995, 1e-9);
    EXPECT_NEAR(window.gt[1], 0.001, 1e-12);

    for (int b = 0; b < 4; b++)
        for (int y = 0; y < 20; y++)
            for (int x = 0; x < 20; x++)
                EXPECT_EQ(window.at(b, x, y), whole.at(b, x + 10, y + 5)) << "band " << b + 1 << " " << x << "," << y;
    // The block of NoData (columns 12 and 13, rows 6 and 7) is in the window, transparent
    EXPECT_EQ(window.at(3, 2, 1), 0);
    EXPECT_EQ(window.at(3, 3, 2), 0);
    EXPECT_EQ(window.at(3, 4, 1), 255);
}

TEST_F(ExportStyledBand, aDemWithoutAColormapStaysOneGreyByteBand) {
    TestArea ta(TEST_NAME);
    const fs::path dem = makeDem(ta.getPath("dem.tif"));
    const fs::path out = ta.getPath("grey.tif");
    ASSERT_EQ(exportAs(dem, out, nullptr, nullptr, nullptr, "0,500"), DDBERR_NONE) << DDBGetLastError();

    const Picture p = readPicture(out);
    EXPECT_EQ(p.bands, 1);
    EXPECT_EQ(p.type, GDT_Byte);
    EXPECT_EQ(p.at(0, 50, 0), static_cast<uint8_t>(255.0 * 500.0 / 500.0));  // 500 is the top of the range
}

TEST_F(ExportStyledBand, anRgbRasterWithAColormapAndNoFormulaStaysThreeBands) {
    TestArea ta(TEST_NAME);
    const fs::path rgb = makeRgb(ta.getPath("rgb.tif"));
    const fs::path plain = ta.getPath("plain.tif");
    const fs::path colormap = ta.getPath("colormap.tif");
    ASSERT_EQ(exportAs(rgb, plain, nullptr, nullptr, nullptr, nullptr), DDBERR_NONE) << DDBGetLastError();
    ASSERT_EQ(exportAs(rgb, colormap, nullptr, nullptr, "viridis", nullptr), DDBERR_NONE) << DDBGetLastError();

    const Picture a = readPicture(plain);
    const Picture b = readPicture(colormap);
    EXPECT_EQ(b.bands, 3);
    EXPECT_EQ(b.type, GDT_Byte);
    for (int band = 0; band < 3; band++) EXPECT_EQ(a.pixels[band], b.pixels[band]) << "band " << band + 1;

    // Two bands of an RGB raster are no single band either
    const fs::path two = ta.getPath("two.tif");
    ASSERT_EQ(exportAs(rgb, two, "1,2", nullptr, "viridis", nullptr), DDBERR_NONE) << DDBGetLastError();
    EXPECT_EQ(readPicture(two).bands, 2);
}

TEST_F(ExportStyledBand, oneBandChosenOfARasterOfSeveralIsStyledAndTheSameBandThreeTimesToo) {
    TestArea ta(TEST_NAME);
    const fs::path rgb = makeRgb(ta.getPath("rgb.tif"));
    const fs::path one = ta.getPath("one.tif");
    const fs::path three = ta.getPath("three.tif");
    ASSERT_EQ(exportAs(rgb, one, "2", nullptr, "viridis", "0,255"), DDBERR_NONE) << DDBGetLastError();
    ASSERT_EQ(exportAs(rgb, three, "2,2,2", nullptr, "viridis", "0,255"), DDBERR_NONE) << DDBGetLastError();

    const Picture a = readPicture(one);
    const Picture b = readPicture(three);
    ASSERT_EQ(a.bands, 4);
    ASSERT_EQ(b.bands, 4);
    for (int band = 0; band < 4; band++) EXPECT_EQ(a.pixels[band], b.pixels[band]);
    // The second band is (3x + 2y + 80) % 256 at (5, 3): 101
    const ddb::ColormapEntry expected = colourOf("viridis", 101.0f, 0.0f, 255.0f);
    EXPECT_EQ(a.at(0, 5, 3), expected.r);
    EXPECT_EQ(a.at(1, 5, 3), expected.g);
    EXPECT_EQ(a.at(2, 5, 3), expected.b);
}

TEST_F(ExportStyledBand, anUnknownColormapOfAStyledBandIsRefusedAndLeavesNoFile) {
    TestArea ta(TEST_NAME);
    const fs::path dem = makeDem(ta.getPath("dem.tif"));
    const fs::path out = ta.getPath("styled.tif");
    EXPECT_NE(exportAs(dem, out, nullptr, nullptr, "no-such-colormap", "0,500"), DDBERR_NONE);
    EXPECT_FALSE(fs::exists(out));
}

}  // namespace
