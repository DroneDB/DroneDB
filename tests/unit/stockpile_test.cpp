/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"

#include "stockpile.h"
#include "volume.h"
#include "ddb.h"
#include "gdal_inc.h"
#include "exceptions.h"
#include "mathutil.h"

#include "json.h"
#include "raster_test_utils.h"
#include "synthetic_dem_support.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace ddb;

// Create a synthetic georeferenced Float32 DEM with constant elevation (flat).
fs::path createSyntheticFlatDem(const fs::path &rasterPath, int w, int h,
                                 double originLon, double originLat,
                                 double pixelSizeDeg) {
    GDALDriverH drv = GDALGetDriverByName("GTiff");
    if (!drv) throw std::runtime_error("No GTiff driver");

    char **opts = nullptr;
    opts = CSLSetNameValue(opts, "COMPRESS", "LZW");
    GDALDatasetH hDs = GDALCreate(drv, rasterPath.string().c_str(),
                                  w, h, 1, GDT_Float32, opts);
    CSLDestroy(opts);
    if (!hDs) throw std::runtime_error("Cannot create synthetic DEM");

    double gt[6] = {originLon, pixelSizeDeg, 0.0,
                    originLat, 0.0, -pixelSizeDeg};
    GDALSetGeoTransform(hDs, gt);

    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 4326);
    char *wkt = nullptr;
    OSRExportToWkt(srs, &wkt);
    GDALSetProjection(hDs, wkt);
    CPLFree(wkt);
    OSRDestroySpatialReference(srs);

    std::vector<float> row(static_cast<size_t>(w), 0.0f); // flat = all zeros
    GDALRasterBandH band = GDALGetRasterBand(hDs, 1);
    for (int y = 0; y < h; y++) {
        GDALRasterIO(band, GF_Write, 0, y, w, 1,
                     row.data(), w, 1, GDT_Float32, 0, 0);
    }

    GDALSetRasterNoDataValue(band, -9999.0);
    GDALFlushCache(hDs);
    GDALClose(hDs);
    return rasterPath;
}

// Create a synthetic DEM with a conical mound on flat ground: z = max(0, R - sqrt(x^2 + y^2)),
// R a quarter of the raster (in pixels), so that there is ground all around the mound
fs::path createSyntheticMoundDem(const fs::path &rasterPath, int w, int h,
                                  double originLon, double originLat,
                                  double pixelSizeDeg) {
    GDALDriverH drv = GDALGetDriverByName("GTiff");
    if (!drv) throw std::runtime_error("No GTiff driver");

    char **opts = nullptr;
    opts = CSLSetNameValue(opts, "COMPRESS", "LZW");
    GDALDatasetH hDs = GDALCreate(drv, rasterPath.string().c_str(),
                                  w, h, 1, GDT_Float32, opts);
    CSLDestroy(opts);
    if (!hDs) throw std::runtime_error("Cannot create synthetic DEM");

    double gt[6] = {originLon, pixelSizeDeg, 0.0,
                    originLat, 0.0, -pixelSizeDeg};
    GDALSetGeoTransform(hDs, gt);

    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 4326);
    char *wkt = nullptr;
    OSRExportToWkt(srs, &wkt);
    GDALSetProjection(hDs, wkt);
    CPLFree(wkt);
    OSRDestroySpatialReference(srs);

    const double R = std::min(static_cast<double>(w), static_cast<double>(h)) / 4.0;
    std::vector<float> row(static_cast<size_t>(w));
    GDALRasterBandH band = GDALGetRasterBand(hDs, 1);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            double dx = static_cast<double>(x) - static_cast<double>(w) / 2.0;
            double dy = static_cast<double>(y) - static_cast<double>(h) / 2.0;
            row[x] = std::max(0.0f, static_cast<float>(R - std::sqrt(dx * dx + dy * dy)));
        }
        GDALRasterIO(band, GF_Write, 0, y, w, 1,
                     row.data(), w, 1, GDT_Float32, 0, 0);
    }

    GDALSetRasterNoDataValue(band, -9999.0);
    GDALFlushCache(hDs);
    GDALClose(hDs);
    return rasterPath;
}

// ---- Test 1: No-pile case (flat DEM) ----------------------------------------

TEST(stockpile, noPileFlatDem) {
    TestArea ta(TEST_NAME);
    const int w = 50, h = 50;
    const double originLon = 10.0;
    const double originLat = 45.0;
    const double pix = 0.0001; // ~11 m at this latitude

    fs::path dem = createSyntheticFlatDem(ta.getPath("dem_flat.tif"),
                                           w, h, originLon, originLat, pix);

    // Click at center - should find no stockpile on a flat DEM
    EXPECT_THROW(detectStockpileJson(dem.string(),
                                      originLat - (h / 2.0) * pix,
                                      originLon,
                                      50.0, // 50m radius
                                      0.5f),
                 AppException);
}

// ---- Test 2: Simple synthetic mound detection -------------------------------

TEST(stockpile, moundDetection) {
    TestArea ta(TEST_NAME);
    const int w = 80, h = 80;
    const double originLon = 10.0;
    const double originLat = 45.0;
    const double pix = 0.0001; // ~11 m at this latitude

    fs::path dem = createSyntheticMoundDem(ta.getPath("dem_mound.tif"),
                                            w, h, originLon, originLat, pix);

    // Click at mound center
    const double centerLat = originLat - (h / 2.0) * pix;
    const double centerLon = originLon + (w / 2.0) * pix;

    // The search radius takes in the ground around the mound (20 pixels of ~8 x 11 m)
    std::string out = detectStockpileJson(dem.string(),
                                           centerLat,
                                           centerLon,
                                           400.0,
                                           0.5f);
    auto j = json::parse(out);

    // Verify JSON schema
    EXPECT_TRUE(j.contains("polygon"));
    EXPECT_TRUE(j.contains("estimatedVolume"));
    EXPECT_TRUE(j.contains("confidence"));
    EXPECT_TRUE(j.contains("baseElevation"));
    EXPECT_TRUE(j.contains("basePlaneMethod"));
    EXPECT_EQ(j["basePlaneMethod"].get<std::string>(), "auto");
    EXPECT_TRUE(j.contains("searchRadius"));
    EXPECT_TRUE(j.contains("sensitivityUsed"));
    EXPECT_TRUE(j.contains("pixelCount"));

    // Confidence should be > 0 for a clear mound
    EXPECT_GT(j["confidence"].get<double>(), 0.0);
    EXPECT_LE(j["confidence"].get<double>(), 1.0);

    // Estimated volume should be positive (mound above base plane)
    EXPECT_GT(j["estimatedVolume"].get<double>(), 0.0);

    // Pixel count should be > 0
    EXPECT_GT(j["pixelCount"].get<int>(), 0);

    // Polygon should be valid GeoJSON with coordinates
    auto &poly = j["polygon"];
    EXPECT_EQ(poly["type"].get<std::string>(), "Polygon");
    EXPECT_TRUE(poly.contains("coordinates"));
}

// ---- Test 3: DDBDetectStockpile C-API smoke test -----------------------------

TEST(stockpile, cApiSmoke) {
    TestArea ta(TEST_NAME);
    const int w = 80, h = 80;
    const double originLon = 10.0;
    const double originLat = 45.0;
    const double pix = 0.0001;

    fs::path dem = createSyntheticMoundDem(ta.getPath("dem_capi.tif"),
                                            w, h, originLon, originLat, pix);

    const double centerLat = originLat - (h / 2.0) * pix;
    const double centerLon = originLon + (w / 2.0) * pix;

    char *output = nullptr;
    auto err = DDBDetectStockpile(dem.string().c_str(),
                                   centerLat,
                                   centerLon,
                                   400.0, // radius in meters: the mound and the ground around it
                                   0.5f,   // sensitivity
                                   &output);
    EXPECT_EQ(err, DDBERR_NONE);
    ASSERT_NE(output, nullptr);

    auto j = json::parse(std::string(output));
    EXPECT_TRUE(j.contains("polygon"));
    EXPECT_TRUE(j.contains("estimatedVolume"));
    EXPECT_TRUE(j.contains("confidence"));
    EXPECT_GT(j["confidence"].get<double>(), 0.0);

    DDBFree(output);
}

// ---- Test 4: C-API invalid args ----------------------------------------------

TEST(stockpile, cApiInvalidArgs) {
    char *output = nullptr;
    // Invalid radius (<= 0)
    EXPECT_NE(DDBDetectStockpile(nullptr, 45.0, 10.0, -1.0, 0.5f, &output), DDBERR_NONE);
    // Non-existent file
    EXPECT_NE(DDBDetectStockpile("nonexistent.tif", 45.0, 10.0, 50.0, 0.5f, &output), DDBERR_NONE);
}

// ---- Test 5: Detect-all on flat DEM returns empty list ----------------------

TEST(stockpile, detectAllFlatDem) {
    TestArea ta(TEST_NAME);
    const int w = 80, h = 80;
    const double originLon = 10.0;
    const double originLat = 45.0;
    const double pix = 0.0001;

    fs::path dem = createSyntheticFlatDem(ta.getPath("dem_flat_all.tif"),
                                           w, h, originLon, originLat, pix);

    std::string out = detectAllStockpilesJson(dem.string(), 0.5f, 0.0, 50);
    auto j = json::parse(out);
    EXPECT_TRUE(j.contains("stockpiles"));
    EXPECT_TRUE(j["stockpiles"].is_array());
    EXPECT_EQ(j["stockpiles"].size(), 0u);
    EXPECT_EQ(j["totalFound"].get<int>(), 0);
}

// ---- Test 6: Detect-all on single-mound DEM finds the mound -----------------

TEST(stockpile, detectAllSingleMound) {
    TestArea ta(TEST_NAME);
    const int w = 120, h = 120;
    const double originLon = 10.0;
    const double originLat = 45.0;
    const double pix = 0.0001;

    fs::path dem = createSyntheticMoundDem(ta.getPath("dem_one_mound.tif"),
                                            w, h, originLon, originLat, pix);

    std::string out = detectAllStockpilesJson(dem.string(), 0.5f, 0.0, 10);
    auto j = json::parse(out);
    ASSERT_TRUE(j.contains("stockpiles"));
    ASSERT_GE(j["stockpiles"].size(), 1u);

    auto &first = j["stockpiles"][0];
    EXPECT_TRUE(first.contains("polygon"));
    EXPECT_TRUE(first.contains("estimatedVolume"));
    EXPECT_TRUE(first.contains("confidence"));
    EXPECT_TRUE(first.contains("baseElevation"));
    EXPECT_TRUE(first.contains("centroid"));
    EXPECT_TRUE(first.contains("areaM2"));
    EXPECT_TRUE(first.contains("pixelCount"));
    EXPECT_TRUE(first.contains("id"));
    EXPECT_GT(first["estimatedVolume"].get<double>(), 0.0);
    EXPECT_GT(first["areaM2"].get<double>(), 0.0);
    EXPECT_GE(first["confidence"].get<double>(), 0.0);
    EXPECT_LE(first["confidence"].get<double>(), 1.0);

    auto &poly = first["polygon"];
    EXPECT_EQ(poly["type"].get<std::string>(), "Polygon");
    ASSERT_TRUE(poly["coordinates"].is_array());
    ASSERT_GE(poly["coordinates"].size(), 1u);
    ASSERT_GE(poly["coordinates"][0].size(), 4u); // closed ring with >=3 unique points
}

// ---- Test 7: maxResults is honoured -----------------------------------------

TEST(stockpile, detectAllRespectsMaxResults) {
    TestArea ta(TEST_NAME);
    const int w = 120, h = 120;
    const double originLon = 10.0;
    const double originLat = 45.0;
    const double pix = 0.0001;

    fs::path dem = createSyntheticMoundDem(ta.getPath("dem_max_results.tif"),
                                            w, h, originLon, originLat, pix);

    std::string out = detectAllStockpilesJson(dem.string(), 0.5f, 0.0, 1);
    auto j = json::parse(out);
    EXPECT_LE(j["stockpiles"].size(), 1u);
}

// ---- Test 8: minAreaM2 filters out tiny components --------------------------

TEST(stockpile, detectAllRespectsMinArea) {
    TestArea ta(TEST_NAME);
    const int w = 120, h = 120;
    const double originLon = 10.0;
    const double originLat = 45.0;
    const double pix = 0.0001;

    fs::path dem = createSyntheticMoundDem(ta.getPath("dem_minarea.tif"),
                                            w, h, originLon, originLat, pix);

    // Use a very large min area (1e9 m^2) to filter everything out.
    std::string out = detectAllStockpilesJson(dem.string(), 0.5f, 1e9, 50);
    auto j = json::parse(out);
    EXPECT_EQ(j["stockpiles"].size(), 0u);
}

// ---- Test 9: C API smoke for DDBDetectAllStockpiles -------------------------

TEST(stockpile, detectAllCApiSmoke) {
    TestArea ta(TEST_NAME);
    const int w = 120, h = 120;
    const double originLon = 10.0;
    const double originLat = 45.0;
    const double pix = 0.0001;

    fs::path dem = createSyntheticMoundDem(ta.getPath("dem_capi_all.tif"),
                                            w, h, originLon, originLat, pix);

    char *output = nullptr;
    auto err = DDBDetectAllStockpiles(dem.string().c_str(), 0.5f, 0.0, 10, &output);
    EXPECT_EQ(err, DDBERR_NONE);
    ASSERT_NE(output, nullptr);

    auto j = json::parse(std::string(output));
    EXPECT_TRUE(j.contains("stockpiles"));
    EXPECT_TRUE(j.contains("totalFound"));
    DDBFree(output);

    // Invalid args
    EXPECT_NE(DDBDetectAllStockpiles(nullptr, 0.5f, 0.0, 10, &output), DDBERR_NONE);
    EXPECT_NE(DDBDetectAllStockpiles("does_not_exist.tif", 0.5f, 0.0, 10, &output), DDBERR_NONE);
}

// ---- Piles on sloping ground --------------------------------------------------------------------
//
// A DEM in UTM with 20 cm pixels, 80 x 80 m: a plane that rises `slopeDeg` towards the south-east
// (the rows go south), cones standing on it, +-2 cm of noise and patches of nodata. The volume of a
// cone above the plane is pi r^2 h / 3, whatever the slope under it.

constexpr double kSlopePixel = 0.2;
constexpr int kSlopeSize = 400;
const golden::DemSpec kSlopeSpec{"slope", 32632, 578000.0, 4985000.0, kSlopePixel, kSlopeSize};

struct Cone {
    double east, south; // meters from the upper-left corner of the DEM
    double radius, height;
    double volume() const { return kPi * radius * radius * height / 3.0; }
};

struct Patch {
    double east, south, size; // a square of nodata, meters
};

// Deterministic noise in [-0.02, 0.02] m
double noiseAt(int x, int y) {
    uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return (static_cast<double>(h % 2001u) / 1000.0 - 1.0) * 0.02;
}

fs::path makeSlopeDem(const fs::path &path, double slopeDeg, const std::vector<Cone> &cones,
                      const std::vector<Patch> &patches = {}) {
    testutils::GeoRasterSpec spec;
    spec.width = spec.height = kSlopeSize;
    spec.bands = 1;
    spec.type = GDT_Float32;
    spec.epsg = kSlopeSpec.epsg;
    spec.originX = kSlopeSpec.originX;
    spec.originY = kSlopeSpec.originY;
    spec.pixelSize = kSlopePixel;
    const double rise = std::tan(slopeDeg * kPi / 180.0);
    spec.value = [=](int, int x, int y) {
        const double e = (x + 0.5) * kSlopePixel, s = (y + 0.5) * kSlopePixel;
        for (const Patch &p : patches) {
            if (e >= p.east && e < p.east + p.size && s >= p.south && s < p.south + p.size) return golden::kNodata;
        }
        double z = 100.0 + rise * (0.8 * e + 0.6 * s) + noiseAt(x, y);
        for (const Cone &c : cones)
            z += c.height * std::max(0.0, 1.0 - std::hypot(e - c.east, s - c.south) / c.radius);
        return z;
    };
    testutils::createGeoRaster(path, spec);
    GDALDatasetH ds = GDALOpen(path.string().c_str(), GA_Update);
    if (!ds) throw std::runtime_error("Cannot open " + path.string());
    GDALSetRasterNoDataValue(GDALGetRasterBand(ds, 1), golden::kNodata);
    GDALClose(ds);
    return path;
}

// The longitude and latitude of a point `east`, `south` meters from the upper-left corner
std::pair<double, double> lonLatAt(double east, double south) {
    return golden::lonLatOfPixel(kSlopeSpec, east / kSlopePixel, south / kSlopePixel);
}

json detectAt(const fs::path &dem, double east, double south, double radius = 30.0, float sensitivity = 0.5f) {
    const auto [lon, lat] = lonLatAt(east, south);
    return json::parse(detectStockpileJson(dem.string(), lat, lon, radius, sensitivity));
}

// The area of the footprint, m^2
double footprintArea(const json &pile) {
    return pile["pixelCount"].get<double>() * kSlopePixel * kSlopePixel;
}

// A pile on a hillside is the cone, not the hillside: the base the detection measures it against
// is the plane of the ground around it, which takes the tilt of the slope. Against a level base
// (the mean of the border of the search window) everything uphill of the mean counted as pile.
TEST(stockpile, aPileOnASlopeIsMeasuredAboveTheSlope) {
    TestArea ta(TEST_NAME);
    const Cone cone{40.0, 40.0, 6.0, 2.5};
    // Nodata on the flank of the pile and on the ground around it
    const std::vector<Patch> patches{{41.0, 42.0, 0.6}, {47.0, 38.0, 1.0}, {30.0, 33.0, 1.5}};
    for (double slope : {5.0, 15.0, 25.0}) {
        SCOPED_TRACE(slope);
        const fs::path dem = makeSlopeDem(ta.getPath("slope.tif"), slope, {cone}, patches);
        for (float sensitivity : {0.3f, 0.5f, 0.8f}) {
            SCOPED_TRACE(sensitivity);
            const json pile = detectAt(dem, 40.1, 39.9, 30.0, sensitivity);
            // The nodata on the flank is no volume: a bit less than the cone
            EXPECT_NEAR(pile["estimatedVolume"].get<double>(), cone.volume(), cone.volume() * 0.1);
            const double disc = kPi * cone.radius * cone.radius;
            EXPECT_GT(footprintArea(pile), disc * 0.7);
            EXPECT_LT(footprintArea(pile), disc * 1.5);
            EXPECT_EQ(pile["basePlaneMethod"].get<std::string>(), "auto");
            // The base under the pile is the slope at its centre
            const double rise = std::tan(slope * kPi / 180.0);
            EXPECT_NEAR(pile["baseElevation"].get<double>(), 100.0 + rise * (0.8 * 40.0 + 0.6 * 40.0), 0.25);
        }
    }
}

// On flat ground the pile is measured whole: the smoothing of the rise no longer eats its volume
TEST(stockpile, aPileOnFlatGroundIsMeasuredWhole) {
    TestArea ta(TEST_NAME);
    const Cone cone{40.0, 40.0, 6.0, 2.5};
    const fs::path dem = makeSlopeDem(ta.getPath("flat.tif"), 0.0, {cone});
    const json pile = detectAt(dem, 40.0, 40.0);
    EXPECT_NEAR(pile["estimatedVolume"].get<double>(), cone.volume(), cone.volume() * 0.05);
    EXPECT_NEAR(pile["baseElevation"].get<double>(), 100.0, 0.05);
}

// Two piles 2 m apart on a slope: a click on either is that pile alone
TEST(stockpile, twoPilesCloseTogetherAreTwoPiles) {
    TestArea ta(TEST_NAME);
    const Cone small{33.0, 40.0, 5.0, 2.0};
    const Cone large{46.0, 40.0, 6.0, 2.5};
    const fs::path dem = makeSlopeDem(ta.getPath("two.tif"), 15.0, {small, large});
    for (const Cone &cone : {small, large}) {
        SCOPED_TRACE(cone.east);
        const json pile = detectAt(dem, cone.east, cone.south);
        EXPECT_NEAR(pile["estimatedVolume"].get<double>(), cone.volume(), cone.volume() * 0.1);
        EXPECT_LT(footprintArea(pile), kPi * cone.radius * cone.radius * 1.5);
    }
}

// A click on the bare slope finds no pile: the slope is not one. The click is near the high
// corner of the DEM, where the search window is cut by the edge of the raster: its border was
// mostly lower than the click, which the level base took for a pile.
TEST(stockpile, aClickOnABareSlopeFindsNoPile) {
    TestArea ta(TEST_NAME);
    const fs::path dem = makeSlopeDem(ta.getPath("bare.tif"), 15.0, {{20.0, 20.0, 6.0, 2.5}}, {{70.0, 76.0, 1.0}});
    const auto [lon, lat] = lonLatAt(74.0, 72.0);
    for (float sensitivity : {0.3f, 0.5f, 0.8f}) {
        SCOPED_TRACE(sensitivity);
        char *out = nullptr;
        EXPECT_EQ(DDBDetectStockpile(dem.string().c_str(), lat, lon, 30.0, sensitivity, &out), DDBERR_INVALIDARGS);
        EXPECT_EQ(out, nullptr);
        EXPECT_STREQ(DDBGetLastError(), "No stockpile detected at this location");
    }
}

// The full scan of a hillside finds its piles, not the hillside: the trend of the terrain is a
// local plane, which a tilted surface follows to the edges of the data
TEST(stockpile, detectAllOnASlopeFindsThePilesNotTheSlope) {
    TestArea ta(TEST_NAME);
    const Cone first{20.0, 20.0, 5.0, 2.0};
    const Cone second{55.0, 55.0, 6.0, 2.5};
    for (double slope : {5.0, 25.0}) {
        SCOPED_TRACE(slope);
        const fs::path dem = makeSlopeDem(ta.getPath("all.tif"), slope, {first, second}, {{70.0, 5.0, 3.0}});
        const json all = json::parse(detectAllStockpilesJson(dem.string(), 0.5f, 1.0, 10));
        ASSERT_EQ(all["totalFound"].get<int>(), 2) << all.dump();
        for (const auto &pile : all["stockpiles"])
            EXPECT_LT(pile["areaM2"].get<double>(), kPi * 6.0 * 6.0 * 1.5);
        // Sorted by volume: the larger cone first
        const auto [lon, lat] = lonLatAt(second.east, second.south);
        EXPECT_NEAR(all["stockpiles"][0]["centroid"][0].get<double>(), lon, 1e-5);
        EXPECT_NEAR(all["stockpiles"][0]["centroid"][1].get<double>(), lat, 1e-5);
    }
}

// ---- A landfill mound -----------------------------------------------------------------------
//
// The 9 m mound of the audit's landfill DEM (FR-MAP-01: about 3,000 m^2 and 8,100-10,100 m^3 within
// a hand-drawn outline), modelled where the DEM itself is not at hand: a broad, nearly level top
// 9 m across with a 1 m hump where one clicks, concave flanks down to a foot 31 m from the top,
// on ground that rises 1.5 degrees with +-3 cm of noise, between a smaller pile and a long ridge.
// A ring on that top hardly descends: the detection took it for the ground and the hump for the
// pile (a 139 m^2 patch of the summit, at 76% confidence).

constexpr double kLandfillPixel = 0.1;
constexpr int kLandfillSize = 1200; // 120 m
const golden::DemSpec kLandfillSpec{"landfill", 32632, 578000.0, 4985000.0, kLandfillPixel, kLandfillSize};
constexpr double kMoundEast = 60.0, kMoundSouth = 60.0; // its top, meters from the upper-left corner

// The height of the mound above the ground, `d` meters from its top
double moundHeight(double d) {
    constexpr double kTop = 9.0, kFoot = 31.0, kShoulder = 8.0, kCrown = 0.005;
    constexpr double kHump = 1.0, kHumpRadius = 4.0;
    double h = 0.0;
    if (d < kTop) h = kShoulder + kCrown * (kTop * kTop - d * d);
    else if (d < kFoot) h = kShoulder * std::pow(1.0 - (d - kTop) / (kFoot - kTop), 1.5);
    return h + kHump * std::max(0.0, 1.0 - d / kHumpRadius);
}

double distanceToSegment(double e, double s, double e0, double s0, double e1, double s1) {
    const double ve = e1 - e0, vs = s1 - s0;
    const double t = std::clamp(((e - e0) * ve + (s - s0) * vs) / (ve * ve + vs * vs), 0.0, 1.0);
    return std::hypot(e - (e0 + t * ve), s - (s0 + t * vs));
}

// The height of the ridge above the ground: 30 m long, 10 m wide, 2.5 m high
double ridgeHeight(double e, double s) {
    return 2.5 * std::max(0.0, 1.0 - distanceToSegment(e, s, 20.0, 105.0, 50.0, 108.0) / 5.0);
}

fs::path makeLandfillDem(const fs::path &path) {
    testutils::GeoRasterSpec spec;
    spec.width = spec.height = kLandfillSize;
    spec.bands = 1;
    spec.type = GDT_Float32;
    spec.epsg = kLandfillSpec.epsg;
    spec.originX = kLandfillSpec.originX;
    spec.originY = kLandfillSpec.originY;
    spec.pixelSize = kLandfillPixel;
    const double rise = std::tan(1.5 * kPi / 180.0);
    spec.value = [=](int, int x, int y) {
        const double e = (x + 0.5) * kLandfillPixel, s = (y + 0.5) * kLandfillPixel;
        double z = 100.0 + rise * (0.8 * e + 0.6 * s) + 1.5 * noiseAt(x, y);
        z += moundHeight(std::hypot(e - kMoundEast, s - kMoundSouth));
        z += 3.0 * std::max(0.0, 1.0 - std::hypot(e - 105.0, s - 60.0) / 7.0);                 // a smaller pile
        z += ridgeHeight(e, s);
        return z;
    };
    testutils::createGeoRaster(path, spec);
    return path;
}

json detectOnLandfill(const fs::path &dem, double radius, float sensitivity, double east = kMoundEast,
                      double south = kMoundSouth) {
    const auto [lon, lat] = golden::lonLatOfPixel(kLandfillSpec, east / kLandfillPixel, south / kLandfillPixel);
    return json::parse(detectStockpileJson(dem.string(), lat, lon, radius, sensitivity));
}

double landfillArea(const json &pile) {
    return pile["pixelCount"].get<double>() * kLandfillPixel * kLandfillPixel;
}

// The volume of a pile of the landfill, summed over the pixels: its height at `east`, `south`
template <typename Height>
double landfillVolume(Height height) {
    double v = 0.0;
    for (int y = 0; y < kLandfillSize; y++) {
        for (int x = 0; x < kLandfillSize; x++)
            v += height((x + 0.5) * kLandfillPixel, (y + 0.5) * kLandfillPixel);
    }
    return v * kLandfillPixel * kLandfillPixel;
}

double moundVolume() {
    return landfillVolume([](double e, double s) { return moundHeight(std::hypot(e - kMoundEast, s - kMoundSouth)); });
}

// The best-fit volume within a hand-drawn outline: a 24-sided polygon 32 m around the top, as the
// audit drew it
double handDrawnVolume(const fs::path &dem) {
    json ring = json::array();
    for (int k = 0; k <= 24; k++) {
        const double a = 2.0 * kPi * (k % 24) / 24.0;
        const auto [lon, lat] = golden::lonLatOfPixel(kLandfillSpec, (kMoundEast + 32.0 * std::cos(a)) / kLandfillPixel,
                                                      (kMoundSouth + 32.0 * std::sin(a)) / kLandfillPixel);
        ring.push_back({lon, lat});
    }
    const json polygon{{"type", "Polygon"}, {"coordinates", json::array({ring})}};
    return json::parse(calculateVolumeJson(dem.string(), polygon.dump(), "best_fit", 0.0))["netVolume"].get<double>();
}

// A click on the top of the mound, at the default sensitivity and radius, outlines its foot: the
// whole mound, not the hump on its top
TEST(stockpile, aClickOnTheTopOfAMoundFindsItsFoot) {
    TestArea ta(TEST_NAME);
    const fs::path dem = makeLandfillDem(ta.getPath("landfill.tif"));
    const json pile = detectOnLandfill(dem, 50.0, 0.5f);
    const double volume = pile["estimatedVolume"].get<double>();
    EXPECT_GE(landfillArea(pile), 2700.0);
    EXPECT_LE(landfillArea(pile), 3300.0);
    EXPECT_GE(volume, 8100.0);
    EXPECT_LE(volume, 10100.0);
    EXPECT_NEAR(volume, moundVolume(), moundVolume() * 0.05);
    const double drawn = handDrawnVolume(dem);
    EXPECT_NEAR(volume, drawn, drawn * 0.1);
    // Its ground is around it: a sure outline
    EXPECT_GE(pile["confidence"].get<double>(), 0.7);
}

// A higher sensitivity finds no less of the mound: it was 3,012 m^3 at 0, 137 m^3 at 0.4 and
// 8,741 m^3 at 0.7 on the audit's landfill
TEST(stockpile, theVolumeDoesNotDecreaseWhenTheSensitivityIncreases) {
    TestArea ta(TEST_NAME);
    const fs::path dem = makeLandfillDem(ta.getPath("landfill.tif"));
    double previous = 0.0;
    for (float sensitivity : {0.0f, 0.25f, 0.4f, 0.5f, 0.6f, 0.7f, 1.0f}) {
        SCOPED_TRACE(sensitivity);
        const double volume = detectOnLandfill(dem, 50.0, sensitivity)["estimatedVolume"].get<double>();
        EXPECT_GE(volume, 8100.0);
        EXPECT_LE(volume, 10100.0);
        EXPECT_GE(volume, previous);
        previous = volume;
    }
}

// A search radius that cannot hold the mound outlines its top only: that outline does not reach
// the foot of the pile, and its confidence says so
TEST(stockpile, anOutlineOfTheTopOnlyHasALowConfidence) {
    TestArea ta(TEST_NAME);
    const fs::path dem = makeLandfillDem(ta.getPath("landfill.tif"));
    const json pile = detectOnLandfill(dem, 20.0, 0.5f);
    EXPECT_LT(landfillArea(pile), 300.0);
    EXPECT_LT(pile["confidence"].get<double>(), 0.5);
}

// A click on a long pile finds all of it, from end to end
TEST(stockpile, aClickOnALongPileFindsItWhole) {
    TestArea ta(TEST_NAME);
    const fs::path dem = makeLandfillDem(ta.getPath("landfill.tif"));
    const json pile = detectOnLandfill(dem, 50.0, 0.5f, 35.0, 106.5);
    const double volume = landfillVolume(ridgeHeight);
    EXPECT_NEAR(pile["estimatedVolume"].get<double>(), volume, volume * 0.1);
    EXPECT_GE(pile["confidence"].get<double>(), 0.7);
}

// ---- Piles on raised ground ---------------------------------------------------------------------
//
// Ground in UTM that rises 2 cm per meter to the east, with +-3 cm of noise, and something on it:
// a pile on a raised pad, on a berm across the window. The pile is measured, not what it stands on.

// A site of `size` x `size` pixels of `pixel` meters, the corner of the landfill DEM
golden::DemSpec siteSpec(double pixel, int size) {
    return golden::DemSpec{"site", 32632, 578000.0, 4985000.0, pixel, size};
}

template <typename Height>
fs::path makeSiteDem(const fs::path &path, const golden::DemSpec &site, Height height) {
    testutils::GeoRasterSpec spec;
    spec.width = spec.height = site.size;
    spec.bands = 1;
    spec.type = GDT_Float32;
    spec.epsg = site.epsg;
    spec.originX = site.originX;
    spec.originY = site.originY;
    spec.pixelSize = site.pixel;
    const double pixel = site.pixel;
    spec.value = [=](int, int x, int y) {
        const double e = (x + 0.5) * pixel, s = (y + 0.5) * pixel;
        return 100.0 + 0.02 * e + 1.5 * noiseAt(x, y) + height(e, s);
    };
    testutils::createGeoRaster(path, spec);
    return path;
}

json detectOnSite(const fs::path &dem, const golden::DemSpec &site, double east, double south, double radius,
                  float sensitivity) {
    const auto [lon, lat] = golden::lonLatOfPixel(site, east / site.pixel, south / site.pixel);
    return json::parse(detectStockpileJson(dem.string(), lat, lon, radius, sensitivity));
}

// A cone on a square pad, both centred 60 m from the corner of the site
struct PileOnPad {
    double padHalf, padHeight; // meters: half the side of the pad, its height (its sides slope at 45 degrees)
    double height, radius;     // of the cone
    double volume() const { return kPi * radius * radius * height / 3.0; }
    double at(double e, double s) const {
        const double inside = std::min({e - (60.0 - padHalf), 60.0 + padHalf - e, s - (60.0 - padHalf), 60.0 + padHalf - s});
        return padHeight * std::clamp(inside / padHeight, 0.0, 1.0) +
               height * std::max(0.0, 1.0 - std::hypot(e - 60.0, s - 60.0) / radius);
    }
};

// A pile on a pad lower than itself is the pile: the pad is its ground. It was taken for a hump on
// the top of the pad, which became the pile (2,323 m^3 for a pile of 113 m^3 on a 1.5 m pad)
TEST(stockpile, aPileOnARaisedPadIsThePileNotThePad) {
    TestArea ta(TEST_NAME);
    const golden::DemSpec site = siteSpec(kLandfillPixel, kLandfillSize);
    for (const PileOnPad &pad : {PileOnPad{20.0, 1.5, 3.0, 6.0}, PileOnPad{20.0, 1.0, 2.0, 5.0},
                                 PileOnPad{20.0, 1.5, 4.0, 6.0}, PileOnPad{10.0, 1.5, 3.0, 6.0},
                                 PileOnPad{20.0, 0.6, 1.0, 4.0}}) {
        SCOPED_TRACE(std::to_string(pad.padHalf) + " " + std::to_string(pad.padHeight) + " " + std::to_string(pad.height));
        const fs::path dem = makeSiteDem(ta.getPath("pad.tif"), site, [&](double e, double s) { return pad.at(e, s); });
        const json pile = detectOnSite(dem, site, 60.0, 60.0, 50.0, 0.5f);
        EXPECT_NEAR(pile["estimatedVolume"].get<double>(), pad.volume(), pad.volume() * 0.15);
        EXPECT_LT(landfillArea(pile), kPi * pad.radius * pad.radius * 1.5);
        EXPECT_GE(pile["confidence"].get<double>(), 0.5);
    }
}

// The pile on a pad is the same pile at every sensitivity and search radius: it was 2,330 m^3 at
// 0.5 and 120 m^3 at 1
TEST(stockpile, aPileOnAPadIsTheSameAtEverySensitivityAndRadius) {
    TestArea ta(TEST_NAME);
    const golden::DemSpec site = siteSpec(kLandfillPixel, kLandfillSize);
    const PileOnPad pad{20.0, 1.5, 3.2, 6.0};
    const fs::path dem = makeSiteDem(ta.getPath("pad.tif"), site, [&](double e, double s) { return pad.at(e, s); });
    double previous = 0.0;
    for (float sensitivity : {0.0f, 0.5f, 1.0f}) {
        SCOPED_TRACE(sensitivity);
        const double volume = detectOnSite(dem, site, 60.0, 60.0, 50.0, sensitivity)["estimatedVolume"].get<double>();
        EXPECT_NEAR(volume, pad.volume(), pad.volume() * 0.15);
        EXPECT_GE(volume, previous);
        previous = volume;
    }
    const double wide = detectOnSite(dem, site, 60.0, 60.0, 50.0, 0.5f)["estimatedVolume"].get<double>();
    const double narrow = detectOnSite(dem, site, 60.0, 60.0, 20.0, 0.5f)["estimatedVolume"].get<double>();
    EXPECT_NEAR(narrow, wide, wide * 0.05);
}

// A pile on a berm that crosses the window: the ground falls away across the berm, not along it,
// so the outline is the foot of the pile and a sure one (the ground beyond the whole window fell
// away by more than the height of the pile, and the confidence was 0.25)
TEST(stockpile, aPileOnABermAcrossTheWindowIsSure) {
    TestArea ta(TEST_NAME);
    const golden::DemSpec site = siteSpec(kLandfillPixel, kLandfillSize);
    const Cone cone{60.0, 60.0, 5.0, 2.5};
    const fs::path dem = makeSiteDem(ta.getPath("berm.tif"), site, [&](double e, double s) {
        return 3.0 * std::clamp((10.0 - std::fabs(s - 60.0)) / 3.0, 0.0, 1.0) +
               cone.height * std::max(0.0, 1.0 - std::hypot(e - cone.east, s - cone.south) / cone.radius);
    });
    const json pile = detectOnSite(dem, site, 60.0, 60.0, 50.0, 0.5f);
    EXPECT_NEAR(pile["estimatedVolume"].get<double>(), cone.volume(), cone.volume() * 0.15);
    EXPECT_GE(pile["confidence"].get<double>(), 0.5);
}

// The default search radius on a fine DEM: the rings far from the click are measured on a coarser
// copy of the window, so their cost does not grow with the resolution (on a 2 cm DEM a click took
// 36 s and 750 MB more when every ring was measured on the window)
TEST(stockpile, aClickWithTheDefaultRadiusOnAFineDemIsQuick) {
    TestArea ta(TEST_NAME);
    const golden::DemSpec site = siteSpec(0.04, 2500); // 100 m
    const Cone cone{50.0, 50.0, 6.0, 3.0};
    const fs::path dem = makeSiteDem(ta.getPath("fine.tif"), site, [&](double e, double s) {
        return cone.height * std::max(0.0, 1.0 - std::hypot(e - cone.east, s - cone.south) / cone.radius);
    });
    const auto start = std::chrono::steady_clock::now();
    const json pile = detectOnSite(dem, site, 50.0, 50.0, 50.0, 0.5f);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_NEAR(pile["estimatedVolume"].get<double>(), cone.volume(), cone.volume() * 0.1);
    EXPECT_LT(elapsed, std::chrono::seconds(8));
}

} // namespace
