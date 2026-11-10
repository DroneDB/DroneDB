/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// One band with a colormap and no formula on the map and in the thumbnails (FR-RAS-01, FR-RAS-26): a DEM that Hub
// opens with a colormap and a rescale (and no formula) is the picture the colormap makes of its values, the one the
// export writes, not the grey stretched tile. The raster is in EPSG:3857 on the grid of one tile, so the pixel
// (x, y) of the tile is the pixel (x, y) of the raster and of its export. The expected colours are those of the
// colormap table at the index the value gives, worked out here by hand.

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ddb.h"
#include "fs.h"
#include "gdal_inc.h"
#include "gtest/gtest.h"
#include "raster_artefacts.h"
#include "raster_ops.h"
#include "raster_stats.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"
#include "user_open_child.h"
#include "tiler.h"
#include "vegetation.h"
#include "vis_render.h"

namespace {

using namespace ddb;

constexpr int kZoom = 18;
constexpr int kTx = 139500;
constexpr int kTy = 160000;
constexpr int kSize = 256;

// A Float32 DEM from -100 (top left) to 282.5 (bottom right): negative values and no NoData
double demValue(int x, int y) { return -100.0 + x + 0.5 * y; }

// The raster of spec on the grid of the tile (kZoom, kTx, kTy)
fs::path onTileGrid(const fs::path& path, testutils::GeoRasterSpec spec) {
    const GlobalMercator mercator(kSize);
    const BoundingBox<Projected2D> b = mercator.tileBounds(kTx, kTy, kZoom);
    spec.width = spec.height = kSize;
    spec.epsg = 3857;
    spec.originX = b.min.x;
    spec.originY = b.max.y;
    spec.pixelSize = mercator.resolution(kZoom);
    return testutils::createGeoRaster(path, spec);
}

fs::path makeDem(const fs::path& path) {
    testutils::GeoRasterSpec spec;
    spec.bands = 1;
    spec.type = GDT_Float32;
    spec.value = [](int, int x, int y) { return demValue(x, y); };
    return onTileGrid(path, spec);
}

void setNodata(const fs::path& path, double nodata) {
    GDALDatasetH ds = GDALOpen(path.string().c_str(), GA_Update);
    ASSERT_NE(ds, nullptr);
    GDALSetRasterNoDataValue(GDALGetRasterBand(ds, 1), nodata);
    GDALClose(ds);
}

// The image of an encoded tile or of a GeoTIFF export: its planes of 8-bit pixels
struct Picture {
    int width = 0, height = 0;
    std::vector<std::vector<uint8_t>> bands;

    uint8_t at(int band, int x, int y) const { return bands[band][static_cast<size_t>(y) * width + x]; }
    // The alpha of a pixel of an RGB(A) picture: a PNG that is opaque everywhere is encoded without one
    uint8_t alphaAt(int x, int y) const { return bands.size() == 4 ? at(3, x, y) : 255; }
};

Picture pictureOf(const testutils::DecodedImage& image) { return {image.width, image.height, image.bands}; }

// The tile (kZoom, kTx, kTy) with the visualisation parameters (NULL leaves one unset)
Picture tile(const fs::path& raster, const char* colormap, const char* rescale, const char* bands = nullptr,
             const DDBRasterOptions* options = nullptr) {
    uint8_t* buffer = nullptr;
    int size = 0;
    const DDBErr err = DDBMemoryTileEx2(raster.string().c_str(), kZoom, kTx, kTy, kSize, false, false, nullptr,
                                        nullptr, bands, nullptr, nullptr, colormap, rescale, options, &buffer, &size);
    EXPECT_EQ(err, DDBERR_NONE) << DDBGetLastError();
    if (err != DDBERR_NONE) return {};
    return pictureOf(testutils::decodeImage(testutils::takeBuffer(buffer, size)));
}

// The plain tile, of the export without visualisation parameters (DDBMemoryTile)
std::vector<uint8_t> plainTileBytes(const fs::path& raster) {
    uint8_t* buffer = nullptr;
    int size = 0;
    EXPECT_EQ(DDBMemoryTile(raster.string().c_str(), kZoom, kTx, kTy, &buffer, &size, kSize, false, false, nullptr),
              DDBERR_NONE)
        << DDBGetLastError();
    return testutils::takeBuffer(buffer, size);
}

std::vector<uint8_t> tileExBytes(const fs::path& raster, const char* colormap, const char* rescale) {
    uint8_t* buffer = nullptr;
    int size = 0;
    EXPECT_EQ(DDBMemoryTileEx(raster.string().c_str(), kZoom, kTx, kTy, kSize, false, false, nullptr, nullptr, nullptr,
                              nullptr, nullptr, colormap, rescale, &buffer, &size),
              DDBERR_NONE)
        << DDBGetLastError();
    return testutils::takeBuffer(buffer, size);
}

// The export of the raster with a colormap and no formula (FR-RAS-26)
Picture exported(const fs::path& raster, const fs::path& out, const char* colormap, const char* rescale) {
    EXPECT_EQ(DDBExportRaster3(raster.string().c_str(), out.string().c_str(), nullptr, nullptr, nullptr, nullptr,
                               colormap, rescale, 0, nullptr, nullptr, nullptr),
              DDBERR_NONE)
        << DDBGetLastError();
    return pictureOf(testutils::decodeImageFile(out));
}

// The colour the table of the colormap gives to v over lo..hi, the rule of the colormap of the formulas
ColormapEntry colourOf(const std::string& id, double v, double lo, double hi) {
    const Colormap* colormap = VegetationEngine::instance().getColormap(id);
    const float normalized =
        std::max(0.0f, std::min(1.0f, (static_cast<float>(v) - static_cast<float>(lo)) /
                                          (static_cast<float>(hi) - static_cast<float>(lo))));
    return colormap->entries[std::max(0, std::min(255, static_cast<int>(normalized * 255.0f)))];
}

// The same RGBA pixels (an opaque PNG has no alpha band: its alpha is 255)
void expectSameRgba(const Picture& a, const Picture& b) {
    ASSERT_EQ(a.width, b.width);
    ASSERT_EQ(a.height, b.height);
    ASSERT_GE(a.bands.size(), 3u);
    ASSERT_GE(b.bands.size(), 3u);
    for (int band = 0; band < 3; band++) EXPECT_EQ(a.bands[band], b.bands[band]) << "band " << band + 1;
    size_t alphaDifferences = 0;
    for (int y = 0; y < a.height; y++)
        for (int x = 0; x < a.width; x++)
            if (a.alphaAt(x, y) != b.alphaAt(x, y)) alphaDifferences++;
    EXPECT_EQ(alphaDifferences, 0u) << "alpha";
}

class StyledBandRender : public ::testing::Test {
protected:
    void SetUp() override { DDBRegisterProcess(false); }
};

TEST_F(StyledBandRender, aDemTileWithAColormapAndARescaleIsTheGradientOfTheColormapAsTheExport) {
    TestArea ta(TEST_NAME, true);
    const fs::path dem = makeDem(ta.getPath("dem.tif"));

    const Picture t = tile(dem, "viridis", "-100,200");
    ASSERT_EQ(t.width, kSize);
    ASSERT_EQ(t.bands.size(), 3u) << "RGB, opaque: the DEM has no NoData";

    // Each pixel is the colour of the table at its stretched value: a gradient, not grey
    std::set<std::tuple<uint8_t, uint8_t, uint8_t>> colours;
    for (int y = 0; y < kSize; y += 5)
        for (int x = 0; x < kSize; x += 5) {
            const ColormapEntry expected = colourOf("viridis", demValue(x, y), -100.0, 200.0);
            ASSERT_EQ(t.at(0, x, y), expected.r) << x << "," << y;
            ASSERT_EQ(t.at(1, x, y), expected.g) << x << "," << y;
            ASSERT_EQ(t.at(2, x, y), expected.b) << x << "," << y;
            colours.emplace(t.at(0, x, y), t.at(1, x, y), t.at(2, x, y));
        }
    EXPECT_GT(colours.size(), 100u);
    const Colormap* viridis = VegetationEngine::instance().getColormap("viridis");
    EXPECT_EQ(t.at(0, 0, 0), viridis->entries[0].r);                // -100, the bottom of the range
    EXPECT_EQ(t.at(2, kSize - 1, kSize - 1), viridis->entries[255].b);  // 282.5, above the top

    // The tile is the export of the same band, pixel for pixel
    expectSameRgba(t, exported(dem, ta.getPath("export.tif"), "viridis", "-100,200"));
}

TEST_F(StyledBandRender, withoutARescaleTheTileIsStretchedOverTheMinimumToTheMaximumAsTheExport) {
    TestArea ta(TEST_NAME, true);
    const fs::path dem = makeDem(ta.getPath("dem.tif"));

    const Picture automatic = tile(dem, "inferno", nullptr);
    ASSERT_GE(automatic.bands.size(), 3u);
    // The minimum is -100 (pixel 0,0) and the maximum 282.5 (pixel 255,255)
    expectSameRgba(automatic, tile(dem, "inferno", "-100,282.5"));
    expectSameRgba(automatic, exported(dem, ta.getPath("export.tif"), "inferno", nullptr));

    const Colormap* inferno = VegetationEngine::instance().getColormap("inferno");
    EXPECT_EQ(automatic.at(0, 0, 0), inferno->entries[0].r);
    EXPECT_EQ(automatic.at(2, kSize - 1, kSize - 1), inferno->entries[255].b);
}

TEST_F(StyledBandRender, thePixelsWithNoDataAreTransparentAndTheNodataOverrideToo) {
    TestArea ta(TEST_NAME, true);
    testutils::GeoRasterSpec spec;
    spec.bands = 1;
    spec.type = GDT_Float32;
    // A block of NoData (-9999) in the columns 20..29 and the rows 40..49
    spec.value = [](int, int x, int y) { return x >= 20 && x < 30 && y >= 40 && y < 50 ? -9999.0 : demValue(x, y); };
    const fs::path dem = onTileGrid(ta.getPath("dem.tif"), spec);
    setNodata(dem, -9999.0);

    const Picture t = tile(dem, "viridis", "-100,200");
    ASSERT_EQ(t.bands.size(), 4u);
    for (int y = 0; y < kSize; y++)
        for (int x = 0; x < kSize; x++) {
            const bool nodata = x >= 20 && x < 30 && y >= 40 && y < 50;
            ASSERT_EQ(t.at(3, x, y), nodata ? 0 : 255) << x << "," << y;
        }
    expectSameRgba(t, exported(dem, ta.getPath("export.tif"), "viridis", "-100,200"));

    // The NoData value of the options hides the pixels that have it: -100 + 10 + 0.5 * 0 at (10, 0)
    DDBRasterOptions options = {};
    options.structSize = sizeof(options);
    options.flags = DDB_RASTER_OPT_NODATA;
    options.nodata = -90.0;
    const Picture overridden = tile(dem, "viridis", "-100,200", nullptr, &options);
    ASSERT_EQ(overridden.bands.size(), 4u);
    EXPECT_EQ(overridden.at(3, 10, 0), 0);
    EXPECT_EQ(overridden.at(3, 11, 0), 255);
}

TEST_F(StyledBandRender, aDemWithoutAColormapKeepsItsGreyTile) {
    TestArea ta(TEST_NAME, true);
    const fs::path dem = makeDem(ta.getPath("dem.tif"));

    const std::vector<uint8_t> plain = plainTileBytes(dem);
    EXPECT_EQ(tileExBytes(dem, nullptr, nullptr), plain);
    EXPECT_EQ(tileExBytes(dem, nullptr, "0,100"), plain) << "the rescale alone draws no colormap";

    const Picture grey = pictureOf(testutils::decodeImage(plain));
    ASSERT_EQ(grey.bands.size(), 1u) << "grey, opaque";
    EXPECT_EQ(grey.at(0, 0, 0), 0);
    EXPECT_EQ(grey.at(0, kSize - 1, kSize - 1), 255);
}

TEST_F(StyledBandRender, anRgbRasterWithAColormapAndNoFormulaKeepsItsTile) {
    TestArea ta(TEST_NAME, true);
    testutils::GeoRasterSpec spec;
    spec.bands = 3;
    spec.type = GDT_Byte;
    spec.value = [](int b, int x, int y) { return (x * 3 + y * 2 + b * 40) % 256; };
    const fs::path rgb = onTileGrid(ta.getPath("rgb.tif"), spec);

    EXPECT_EQ(tileExBytes(rgb, "viridis", nullptr), plainTileBytes(rgb));
    EXPECT_EQ(tileExBytes(rgb, "viridis", "0,255"), plainTileBytes(rgb));

    // One band of it is one band: painted, as the export of that band. The COG that the tiles read of an 8-bit RGB
    // raster is JPEG: the value 101 of the second band at (5, 3) comes back within a few levels
    const Picture second = tile(rgb, "viridis", "0,255", "2");
    ASSERT_EQ(second.bands.size(), 3u) << "RGB of the colormap";
    const ColormapEntry expected = colourOf("viridis", (5 * 3 + 3 * 2 + 80) % 256, 0.0, 255.0);
    EXPECT_NEAR(second.at(0, 5, 3), expected.r, 6);
    EXPECT_NEAR(second.at(1, 5, 3), expected.g, 6);
    EXPECT_NEAR(second.at(2, 5, 3), expected.b, 6);
    expectSameRgba(second, tile(rgb, "viridis", "0,255", "2,2,2"));
}

// A Byte band with a colormap is stretched over its minimum to its maximum as any other band, in the tile as in
// the export (its plain tile is not stretched)
TEST_F(StyledBandRender, aByteBandWithAColormapIsStretchedOverItsRangeAsTheExport) {
    TestArea ta(TEST_NAME, true);
    testutils::GeoRasterSpec spec;
    spec.bands = 1;
    spec.type = GDT_Byte;
    spec.value = [](int, int x, int y) { return 50 + (x + y) * 100 / 510; };
    const fs::path band = onTileGrid(ta.getPath("byte.tif"), spec);

    const Picture t = tile(band, "plasma", nullptr);
    ASSERT_EQ(t.bands.size(), 3u);
    const Colormap* plasma = VegetationEngine::instance().getColormap("plasma");
    EXPECT_EQ(t.at(0, 0, 0), plasma->entries[0].r);                    // 50, the minimum
    EXPECT_EQ(t.at(2, kSize - 1, kSize - 1), plasma->entries[255].b);  // 150, the maximum
    expectSameRgba(t, exported(band, ta.getPath("export.tif"), "plasma", nullptr));
}

TEST_F(StyledBandRender, anUnknownColormapOfAStyledTileIsRefused) {
    TestArea ta(TEST_NAME, true);
    const fs::path dem = makeDem(ta.getPath("dem.tif"));
    uint8_t* buffer = nullptr;
    int size = 0;
    EXPECT_EQ(DDBMemoryTileEx2(dem.string().c_str(), kZoom, kTx, kTy, kSize, false, false, nullptr, nullptr, nullptr,
                               nullptr, nullptr, "no-such-colormap", nullptr, nullptr, &buffer, &size),
              DDBERR_INVALIDARGS);
    EXPECT_EQ(buffer, nullptr);
    // The export of 1.x keeps its codes
    EXPECT_EQ(DDBMemoryTileEx(dem.string().c_str(), kZoom, kTx, kTy, kSize, false, false, nullptr, nullptr, nullptr,
                              nullptr, nullptr, "no-such-colormap", nullptr, &buffer, &size),
              DDBERR_EXCEPTION);
    EXPECT_EQ(buffer, nullptr);
    // As the export of the same band
    EXPECT_EQ(DDBExportRaster3(dem.string().c_str(), ta.getPath("export.tif").string().c_str(), nullptr, nullptr,
                               nullptr, nullptr, "no-such-colormap", nullptr, 0, nullptr, nullptr, nullptr),
              DDBERR_INVALIDARGS);
    // A thumbnail fails too, instead of drawing the band grey
    EXPECT_NE(DDBGenerateMemoryThumbnailEx2(dem.string().c_str(), kSize, nullptr, nullptr, nullptr, nullptr,
                                            "no-such-colormap", nullptr, nullptr, &buffer, &size),
              DDBERR_NONE);
    EXPECT_EQ(buffer, nullptr);
}

// The thumbnail (WebP, lossy) of the same DEM, at the size of the raster
Picture thumbnail(const fs::path& raster, const char* colormap, const char* rescale) {
    uint8_t* buffer = nullptr;
    int size = 0;
    const DDBErr err = DDBGenerateMemoryThumbnailEx2(raster.string().c_str(), kSize, nullptr, nullptr, nullptr, nullptr,
                                                     colormap, rescale, nullptr, &buffer, &size);
    EXPECT_EQ(err, DDBERR_NONE) << DDBGetLastError();
    if (err != DDBERR_NONE) return {};
    return pictureOf(testutils::decodeImage(testutils::takeBuffer(buffer, size)));
}

TEST_F(StyledBandRender, theThumbnailOfADemWithAColormapIsTheColouredPictureOfTheExport) {
    TestArea ta(TEST_NAME, true);
    const fs::path dem = makeDem(ta.getPath("dem.tif"));

    const Picture thumb = thumbnail(dem, "viridis", "-100,200");
    ASSERT_EQ(thumb.width, kSize);
    ASSERT_EQ(thumb.height, kSize);
    ASSERT_GE(thumb.bands.size(), 3u);

    // The colours of the export, but for the loss of the WebP encoding
    const Picture exportPicture = exported(dem, ta.getPath("export.tif"), "viridis", "-100,200");
    double difference = 0;
    for (int b = 0; b < 3; b++)
        for (size_t i = 0; i < thumb.bands[b].size(); i++)
            difference += std::abs(static_cast<int>(thumb.bands[b][i]) - static_cast<int>(exportPicture.bands[b][i]));
    EXPECT_LT(difference / (3.0 * kSize * kSize), 3.0);

    // Dark violet at -100, yellow at the top of the range: not grey
    const Colormap* viridis = VegetationEngine::instance().getColormap("viridis");
    for (const auto& [x, y, entry] : std::vector<std::tuple<int, int, int>>{{2, 2, 1}, {253, 253, 255}}) {
        EXPECT_NEAR(thumb.at(0, x, y), viridis->entries[entry].r, 12) << x << "," << y;
        EXPECT_NEAR(thumb.at(1, x, y), viridis->entries[entry].g, 12) << x << "," << y;
        EXPECT_NEAR(thumb.at(2, x, y), viridis->entries[entry].b, 12) << x << "," << y;
    }

    // Without a colormap the thumbnail stays grey
    const Picture grey = thumbnail(dem, nullptr, "-100,200");
    ASSERT_GE(grey.bands.size(), 3u);
    for (const auto& [x, y] : std::vector<std::pair<int, int>>{{2, 2}, {40, 30}, {253, 253}}) {
        EXPECT_NEAR(grey.at(0, x, y), grey.at(1, x, y), 3) << x << "," << y;
        EXPECT_NEAR(grey.at(1, x, y), grey.at(2, x, y), 3) << x << "," << y;
    }
}

// A tile of the raster's zoom that the raster does not cover (the raster is one tile of its zoom)
DDBErr tileOutside(const fs::path& raster, const char* colormap, const char* bands, std::vector<uint8_t>* bytes) {
    uint8_t* buffer = nullptr;
    int size = 0;
    const DDBErr err = DDBMemoryTileEx2(raster.string().c_str(), kZoom, kTx + 10, kTy, kSize, false, false, nullptr,
                                        nullptr, bands, nullptr, nullptr, colormap, nullptr, nullptr, &buffer, &size);
    *bytes = testutils::takeBuffer(buffer, size);
    return err;
}

// The tile out of the raster keeps the contract of 2.3.0: visualisation parameters (bands) refuse it ("Out of
// bounds", before any statistics are read), and a colormap alone, which 2.3.0 drew as the plain tile, is the plain
// tile: transparent
TEST_F(StyledBandRender, aTileOutsideTheRasterIsRefusedWithBandsAndTransparentWithAColormapAlone) {
    TestArea ta(TEST_NAME, true);
    const fs::path dem = makeDem(ta.getPath("dem.tif"));

    std::vector<uint8_t> bytes;
    EXPECT_NE(tileOutside(dem, "viridis", "1", &bytes), DDBERR_NONE);
    EXPECT_TRUE(bytes.empty());
    EXPECT_NE(std::string(DDBGetLastError()).find("Out of bounds"), std::string::npos) << DDBGetLastError();
    // No statistics were read for it: the sidecar of the raster is not written
    EXPECT_FALSE(fs::exists(statsSidecarOf(dem)));

    ASSERT_EQ(tileOutside(dem, "viridis", nullptr, &bytes), DDBERR_NONE) << DDBGetLastError();
    const Picture outside = pictureOf(testutils::decodeImage(bytes));
    ASSERT_GE(outside.bands.size(), 2u) << "the plain tile: grey and alpha";
    for (const uint8_t alpha : outside.bands.back()) ASSERT_EQ(alpha, 0);

    // The plain tile of the same coordinates is the same picture
    uint8_t* buffer = nullptr;
    int size = 0;
    ASSERT_EQ(DDBMemoryTile(dem.string().c_str(), kZoom, kTx + 10, kTy, &buffer, &size, kSize, false, false, nullptr),
              DDBERR_NONE)
        << DDBGetLastError();
    EXPECT_EQ(bytes, testutils::takeBuffer(buffer, size));
}

// A band with no pixel that has a value has no range to stretch: its tile, its thumbnail and its export are empty
// pictures (all transparent), not errors, as 2.3.0 drew them
void expectAllNodataBandRendersTransparent(const fs::path& raster, const char* bands) {
    const Picture t = tile(raster, "viridis", nullptr, bands);
    ASSERT_EQ(t.bands.size(), 4u);
    for (const uint8_t alpha : t.bands[3]) ASSERT_EQ(alpha, 0);

    uint8_t* buffer = nullptr;
    int size = 0;
    ASSERT_EQ(DDBGenerateMemoryThumbnailEx2(raster.string().c_str(), kSize, nullptr, bands, nullptr, nullptr,
                                            "viridis", nullptr, nullptr, &buffer, &size),
              DDBERR_NONE)
        << DDBGetLastError();
    const Picture thumb = pictureOf(testutils::decodeImage(testutils::takeBuffer(buffer, size)));
    ASSERT_FALSE(thumb.bands.empty());
    if (thumb.bands.size() == 4u) {
        for (const uint8_t alpha : thumb.bands[3]) {
            ASSERT_EQ(alpha, 0);
        }
    }

    const fs::path out = raster.parent_path() / (std::string(bands ? bands : "all") + "-export.tif");
    ASSERT_EQ(DDBExportRaster3(raster.string().c_str(), out.string().c_str(), nullptr, bands, nullptr, nullptr,
                               "viridis", nullptr, 0, nullptr, nullptr, nullptr),
              DDBERR_NONE)
        << DDBGetLastError();
    const Picture e = pictureOf(testutils::decodeImageFile(out));
    ASSERT_EQ(e.bands.size(), 4u);
    for (const uint8_t alpha : e.bands[3]) ASSERT_EQ(alpha, 0);
}

TEST_F(StyledBandRender, aFloatBandOfNodataOnlyHasATransparentTileThumbnailAndExport) {
    TestArea ta(TEST_NAME, true);
    testutils::GeoRasterSpec spec;
    spec.bands = 1;
    spec.type = GDT_Float32;
    spec.value = [](int, int, int) { return -9999.0; };
    const fs::path nothing = onTileGrid(ta.getPath("nothing.tif"), spec);
    setNodata(nothing, -9999.0);

    expectAllNodataBandRendersTransparent(nothing, nullptr);
    expectAllNodataBandRendersTransparent(nothing, "1");
}

TEST_F(StyledBandRender, aByteBandOfNodataOnlyHasATransparentTileThumbnailAndExport) {
    TestArea ta(TEST_NAME, true);
    testutils::GeoRasterSpec spec;
    spec.bands = 1;
    spec.type = GDT_Byte;
    spec.value = [](int, int, int) { return 0.0; };
    const fs::path nothing = onTileGrid(ta.getPath("nothing.tif"), spec);
    setNodata(nothing, 0.0);

    expectAllNodataBandRendersTransparent(nothing, nullptr);
    expectAllNodataBandRendersTransparent(nothing, "1");
}

// A raster larger than the budget of the stretch of a thumbnail (D-041): 2304 x 2304 Float32 pixels in blocks of
// 64 x 64 (36 x 36 blocks), each pixel the column plus the row of its block, but for block (0, 0), which holds 10000.
// The sample of a thumbnail reads the middle block of every 2 x 2 cell of blocks, so never block (0, 0): its range is
// 2 to 70, while the exact range is 1 to 10000.
constexpr int kLargeSize = 2304;
constexpr double kOutlier = 10000.0;

void makeLargeBlockRaster(const fs::path& path) {
    char** opts = CSLSetNameValue(nullptr, "TILED", "YES");
    opts = CSLSetNameValue(opts, "BLOCKXSIZE", "64");
    opts = CSLSetNameValue(opts, "BLOCKYSIZE", "64");
    GDALDatasetH ds =
        GDALCreate(GDALGetDriverByName("GTiff"), path.string().c_str(), kLargeSize, kLargeSize, 1, GDT_Float32, opts);
    CSLDestroy(opts);
    ASSERT_NE(ds, nullptr);
    std::vector<float> row(kLargeSize);
    for (int y = 0; y < kLargeSize; y++) {
        for (int x = 0; x < kLargeSize; x++)
            row[static_cast<size_t>(x)] = x < 64 && y < 64 ? static_cast<float>(kOutlier)
                                                           : static_cast<float>(x / 64 + y / 64);
        ASSERT_EQ(GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Write, 0, y, kLargeSize, 1, row.data(), kLargeSize, 1,
                               GDT_Float32, 0, 0),
                  CE_None);
    }
    GDALClose(ds);
}

// The thumbnail of the band with viridis, as the raster helper renders it
RasterImage styledThumbnail(const fs::path& raster, const std::string& rescale) {
    ThumbVisParams vis;
    vis.colormap = "viridis";
    vis.rescale = rescale;
    return raster::renderThumbnail(raster, {256, vis});
}

std::string rescaleOf(double lo, double hi) {
    std::ostringstream os;
    os << std::setprecision(17) << lo << "," << hi;
    return os.str();
}

void expectSameImage(const RasterImage& a, const RasterImage& b) {
    ASSERT_EQ(a.width, b.width);
    ASSERT_EQ(a.height, b.height);
    EXPECT_TRUE(a.color == b.color);
    EXPECT_TRUE(a.alpha == b.alpha);
}

// The thumbnail of one band with a colormap takes its default stretch as every thumbnail does (D-041): from a bounded
// sample of a large raster whose exact statistics are not known, never kept in the statistics sidecar, and from its
// exact statistics once they are known. The tile keeps the exact statistics.
// The thumbnail is rendered in this process as the raster helper renders it, so the test is the helper (its
// statistics are those of this process)
USER_OPEN_TEST_F(StyledBandRender, theThumbnailOfALargeBandWithAColormapIsStretchedOverTheSampleOfTheThumbnails) {
    // Under the artefacts folder: a file libddb wrote, whose exact statistics go to its sidecar
    fs::create_directories(raster::artefactsDir());
    const fs::path raster = raster::artefactsDir() / (TEST_NAME + "-large.tif");
    const fs::path sidecar = statsSidecarOf(raster);
    fs::remove(raster);
    fs::remove(sidecar);
    makeLargeBlockRaster(raster);
    ASSERT_FALSE(HasFatalFailure());

    auto& provider = RasterStatsProvider::instance();
    provider.clear();
    const auto before = provider.counters();
    const RasterImage sampled = styledThumbnail(raster, "");
    const auto afterThumbnail = provider.counters();

    // One bounded read of the band, no exact pass, nothing in the sidecar
    EXPECT_EQ(afterThumbnail.sampled - before.sampled, 1u);
    EXPECT_EQ(afterThumbnail.computed, before.computed);
    EXPECT_EQ(afterThumbnail.fromSidecar, before.fromSidecar);
    EXPECT_FALSE(fs::exists(sidecar));

    // The thumbnail is the picture of the band over the range of the sample, 2 to 70
    GDALDatasetH ds = GDALOpen(raster.string().c_str(), GA_ReadOnly);
    ASSERT_NE(ds, nullptr);
    const BandStatistics sample = provider.getStretchStatistics(raster.string(), ds, {1})[0];
    EXPECT_TRUE(sample.sampled);
    EXPECT_EQ(sample.min, 2.0);
    EXPECT_EQ(sample.max, 70.0);
    expectSameImage(sampled, styledThumbnail(raster, rescaleOf(sample.min, sample.max)));

    // The tile (and the export) paint the band over its exact statistics, 1 to 10000, which it computes
    const BandStylePlan tilePlan = planBandStyle(raster.string(), ds, 1, "viridis", "", std::nullopt);
    EXPECT_EQ(tilePlan.range.first, 1.0f);
    EXPECT_EQ(tilePlan.range.second, static_cast<float>(kOutlier));
    EXPECT_EQ(provider.counters().computed - afterThumbnail.computed, 1u);

    // Once the exact statistics are known, the thumbnail takes them
    const BandStatistics exact = provider.get(raster.string(), ds, 1);
    GDALClose(ds);
    EXPECT_FALSE(exact.sampled);
    EXPECT_EQ(exact.min, 1.0);
    EXPECT_EQ(exact.max, kOutlier);
    EXPECT_TRUE(fs::exists(sidecar));
    const auto beforeExact = provider.counters();
    const RasterImage exactThumbnail = styledThumbnail(raster, "");
    EXPECT_EQ(provider.counters().sampled, beforeExact.sampled);
    EXPECT_EQ(provider.counters().computed, beforeExact.computed);
    expectSameImage(exactThumbnail, styledThumbnail(raster, rescaleOf(exact.min, exact.max)));
    EXPECT_FALSE(exactThumbnail.color == sampled.color);

    provider.clear();
    fs::remove(raster);
    fs::remove(sidecar);
}

}  // namespace
