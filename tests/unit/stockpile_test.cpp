/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"

#include "stockpile.h"
#include "ddb.h"
#include "gdal_inc.h"
#include "exceptions.h"
#include "mathutil.h"

#include "json.h"
#include "raster_test_utils.h"
#include "synthetic_dem_support.h"

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

} // namespace
