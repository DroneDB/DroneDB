/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"

#include "contour.h"
#include "ddb.h"
#include "exceptions.h"
#include "gdal_inc.h"
#include "json.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace ddb;

// Synthetic DEM with a deterministic linear gradient along the X axis:
// value(x, y) = x (in pixels). Geographic CRS EPSG:4326 so we can read out
// WGS84 coordinates directly without coordinate transforms.
fs::path createGradientDem(const fs::path &rasterPath, int w, int h,
                           double originLon = 10.0,
                           double originLat = 45.0,
                           double pixelSizeDeg = 0.0001) {
    GDALDriverH drv = GDALGetDriverByName("GTiff");
    if (!drv) throw std::runtime_error("No GTiff driver");

    GDALDatasetH hDs = GDALCreate(drv, rasterPath.string().c_str(),
                                  w, h, 1, GDT_Float32, nullptr);
    if (!hDs) throw std::runtime_error("Cannot create DEM");

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

    std::vector<float> row(w);
    GDALRasterBandH band = GDALGetRasterBand(hDs, 1);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) row[x] = static_cast<float>(x);
        GDALRasterIO(band, GF_Write, 0, y, w, 1,
                     row.data(), w, 1, GDT_Float32, 0, 0);
    }
    GDALSetRasterUnitType(band, "m");
    GDALFlushCache(hDs);
    GDALClose(hDs);
    return rasterPath;
}

// ---- Happy path ------------------------------------------------------------

TEST(contour, fixedIntervalProducesFeatures) {
    TestArea ta(TEST_NAME);
    fs::path dem = createGradientDem(ta.getPath("dem_interval.tif"), 100, 50);

    ContourOptions o;
    o.interval = 10.0; // gradient spans 0..99 -> ~10 contour levels

    auto j = json::parse(generateContoursJson(dem.string(), o));

    EXPECT_EQ(j["type"].get<std::string>(), "FeatureCollection");
    EXPECT_GT(j["featureCount"].get<int>(), 0);
    EXPECT_DOUBLE_EQ(j["interval"].get<double>(), 10.0);
    EXPECT_DOUBLE_EQ(j["baseOffset"].get<double>(), 0.0);

    // Every feature must have an `elev` numeric property and a LineString geom.
    for (auto &f : j["features"]) {
        EXPECT_EQ(f["type"].get<std::string>(), "Feature");
        EXPECT_EQ(f["geometry"]["type"].get<std::string>(), "LineString");
        EXPECT_TRUE(f["properties"]["elev"].is_number());
        EXPECT_GE(f["geometry"]["coordinates"].size(), 2u);
    }
}

TEST(contour, targetCountDerivesInterval) {
    TestArea ta(TEST_NAME);
    fs::path dem = createGradientDem(ta.getPath("dem_count.tif"), 100, 50);

    ContourOptions o;
    o.count = 10;

    auto j = json::parse(generateContoursJson(dem.string(), o));

    // Range is ~ [0, 99], so interval ~ 9.9.
    const double interval = j["interval"].get<double>();
    EXPECT_GT(interval, 0.0);
    EXPECT_NEAR(interval, 9.9, 0.5);
    EXPECT_GT(j["featureCount"].get<int>(), 0);
}

TEST(contour, baseOffsetAlignsLevels) {
    TestArea ta(TEST_NAME);
    fs::path dem = createGradientDem(ta.getPath("dem_base.tif"), 100, 50);

    ContourOptions o;
    o.interval = 10.0;
    o.baseOffset = 5.0;

    auto j = json::parse(generateContoursJson(dem.string(), o));
    ASSERT_GT(j["featureCount"].get<int>(), 0);

    // All levels must be at base + k*interval (5, 15, 25, ...).
    for (auto &f : j["features"]) {
        const double e = f["properties"]["elev"].get<double>();
        const double remainder = std::fmod(e - 5.0, 10.0);
        EXPECT_NEAR(remainder, 0.0, 1e-6);
    }
}

TEST(contour, minMaxClipping) {
    TestArea ta(TEST_NAME);
    fs::path dem = createGradientDem(ta.getPath("dem_clip.tif"), 100, 50);

    ContourOptions o;
    o.interval = 10.0;
    o.minElev = 30.0;
    o.maxElev = 70.0;

    auto j = json::parse(generateContoursJson(dem.string(), o));
    ASSERT_GT(j["featureCount"].get<int>(), 0);

    for (auto &f : j["features"]) {
        const double e = f["properties"]["elev"].get<double>();
        EXPECT_GE(e, 30.0);
        EXPECT_LE(e, 70.0);
    }
}

// ---- Error paths -----------------------------------------------------------

TEST(contour, missingIntervalAndCountThrows) {
    TestArea ta(TEST_NAME);
    fs::path dem = createGradientDem(ta.getPath("dem_err1.tif"), 20, 20);

    ContourOptions o; // both unset
    EXPECT_THROW(generateContoursJson(dem.string(), o), InvalidArgsException);
}

TEST(contour, invalidMinMaxThrows) {
    TestArea ta(TEST_NAME);
    fs::path dem = createGradientDem(ta.getPath("dem_err2.tif"), 20, 20);

    ContourOptions o;
    o.interval = 5.0;
    o.minElev = 50.0;
    o.maxElev = 10.0;
    EXPECT_THROW(generateContoursJson(dem.string(), o), InvalidArgsException);
}

TEST(contour, missingRasterThrows) {
    ContourOptions o;
    o.interval = 1.0;
    EXPECT_THROW(generateContoursJson("nonexistent_dem.tif", o), GDALException);
}

TEST(contour, invalidBandThrows) {
    TestArea ta(TEST_NAME);
    fs::path dem = createGradientDem(ta.getPath("dem_err3.tif"), 20, 20);

    ContourOptions o;
    o.interval = 5.0;
    o.bandIndex = 99;
    EXPECT_THROW(generateContoursJson(dem.string(), o), InvalidArgsException);
}

// ---- C API round-trip ------------------------------------------------------

TEST(contour, cApiGenerateContours) {
    TestArea ta(TEST_NAME);
    fs::path dem = createGradientDem(ta.getPath("dem_capi.tif"), 100, 50);

    char *out = nullptr;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    DDBErr err = DDBGenerateContours(dem.string().c_str(),
                                     10.0, 0,
                                     0.0,
                                     nan, nan,
                                     0.0,
                                     1,
                                     &out);
    ASSERT_EQ(err, DDBERR_NONE);
    ASSERT_NE(out, nullptr);

    auto j = json::parse(std::string(out));
    EXPECT_EQ(j["type"].get<std::string>(), "FeatureCollection");
    EXPECT_GT(j["featureCount"].get<int>(), 0);

    DDBFree(out);
}

TEST(contour, cApiInvalidArgsReturnsError) {
    char *out = nullptr;
    // Neither interval (>0) nor count (>0) provided.
    DDBErr err = DDBGenerateContours("anywhere.tif",
                                     0.0, 0,
                                     0.0,
                                     std::numeric_limits<double>::quiet_NaN(),
                                     std::numeric_limits<double>::quiet_NaN(),
                                     0.0,
                                     1,
                                     &out);
    EXPECT_EQ(err, DDBERR_EXCEPTION);
    if (out) DDBFree(out);
}

} // namespace
