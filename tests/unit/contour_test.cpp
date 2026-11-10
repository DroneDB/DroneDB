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
#include <cstdint>
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

// ---- The cap, progress and cancel (FR-RAS-11) -------------------------------

// A DEM of pseudo-random noise (a fixed generator, the same values on every run): every pixel is a
// different level, so the contours are many short lines
fs::path createNoisyDem(const fs::path &rasterPath, int w, int h) {
    GDALDriverH drv = GDALGetDriverByName("GTiff");
    GDALDatasetH hDs = GDALCreate(drv, rasterPath.string().c_str(), w, h, 1, GDT_Float32, nullptr);
    if (!hDs) throw std::runtime_error("Cannot create DEM");
    double gt[6] = {10.0, 0.0001, 0.0, 45.0, 0.0, -0.0001};
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
    uint32_t state = 12345;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            state = state * 1664525u + 1013904223u;
            row[x] = static_cast<float>((state >> 8) % 1000) / 10.0f; // 0.0 .. 99.9
        }
        if (GDALRasterIO(band, GF_Write, 0, y, w, 1, row.data(), w, 1, GDT_Float32, 0, 0) != CE_None)
            throw std::runtime_error("Cannot write DEM");
    }
    GDALFlushCache(hDs);
    GDALClose(hDs);
    return rasterPath;
}

TEST(contour, aNoisyRasterThatHitsTheCapIsTruncated) {
    TestArea ta(TEST_NAME);
    fs::path dem = createNoisyDem(ta.getPath("dem_noise.tif"), 120, 120);

    ContourOptions o;
    o.interval = 1.0;
    o.maxFeatures = 50;

    auto j = json::parse(generateContoursJson(dem.string(), o));
    EXPECT_TRUE(j["truncated"].get<bool>());
    EXPECT_EQ(j["featureCap"].get<int>(), 50);
    EXPECT_EQ(j["featureCount"].get<int>(), 50);
    EXPECT_EQ(j["features"].size(), 50u);
}

TEST(contour, aResultWithinTheCapIsNotTruncated) {
    TestArea ta(TEST_NAME);
    fs::path dem = createGradientDem(ta.getPath("dem_cap_ok.tif"), 100, 50);

    ContourOptions o;
    o.interval = 10.0;

    auto j = json::parse(generateContoursJson(dem.string(), o));
    EXPECT_FALSE(j["truncated"].get<bool>());
    // The cap of a result is 100000 lines
    EXPECT_EQ(j["featureCap"].get<int>(), 100000);
    EXPECT_EQ(j["featureCount"].get<int>(), 9);
}

TEST(contour, aResultOfExactlyTheCapIsNotTruncated) {
    TestArea ta(TEST_NAME);
    fs::path dem = createGradientDem(ta.getPath("dem_cap_edge.tif"), 100, 50);

    ContourOptions o;
    o.interval = 10.0; // the levels 10, 20 .. 90: nine lines
    o.maxFeatures = 9;
    auto exact = json::parse(generateContoursJson(dem.string(), o));
    EXPECT_FALSE(exact["truncated"].get<bool>());
    EXPECT_EQ(exact["featureCount"].get<int>(), 9);

    o.maxFeatures = 8;
    auto cut = json::parse(generateContoursJson(dem.string(), o));
    EXPECT_TRUE(cut["truncated"].get<bool>());
    EXPECT_EQ(cut["featureCap"].get<int>(), 8);
    EXPECT_EQ(cut["featureCount"].get<int>(), 8);
}

TEST(contour, progressGoesFromTheStatisticsToTheLinesAndNeverBack) {
    TestArea ta(TEST_NAME);
    fs::path dem = createNoisyDem(ta.getPath("dem_progress.tif"), 120, 120);

    std::vector<double> fractions;
    std::vector<std::string> phases;
    ContourOptions o;
    o.interval = 1.0;
    o.progress = [&](double fraction, const char *phase) {
        fractions.push_back(fraction);
        if (phases.empty() || phases.back() != phase) phases.push_back(phase);
        return false;
    };
    auto j = json::parse(generateContoursJson(dem.string(), o));
    EXPECT_FALSE(j["truncated"].get<bool>());

    ASSERT_GE(fractions.size(), 4u);
    EXPECT_DOUBLE_EQ(fractions.front(), 0.0);
    EXPECT_DOUBLE_EQ(fractions.back(), 1.0);
    for (size_t i = 1; i < fractions.size(); i++) EXPECT_GE(fractions[i], fractions[i - 1]) << i;
    EXPECT_EQ(phases, (std::vector<std::string>{"statistics", "levels", "lines"}));
}

TEST(contour, aProgressCallbackThatAsksToStopCancelsTheGeneration) {
    TestArea ta(TEST_NAME);
    fs::path dem = createNoisyDem(ta.getPath("dem_stop.tif"), 120, 120);

    ContourOptions o;
    o.interval = 1.0;
    int calls = 0;
    o.progress = [&](double, const char *phase) {
        calls++;
        return std::string(phase) == "levels"; // stop at the first report of the generator
    };
    EXPECT_THROW(generateContoursJson(dem.string(), o), CanceledException);
    EXPECT_LT(calls, 10);
}

TEST(contour, cApiProgressAndCancel) {
    TestArea ta(TEST_NAME);
    fs::path dem = createNoisyDem(ta.getPath("dem_capi2.tif"), 120, 120);
    const double nan = std::numeric_limits<double>::quiet_NaN();

    struct Seen {
        int calls = 0;
        double last = -1;
        bool stop = false;
    } seen;
    auto callback = [](double fraction, const char *, void *user) -> int {
        auto *s = static_cast<Seen *>(user);
        s->calls++;
        s->last = fraction;
        return s->stop ? 1 : 0;
    };

    char *out = nullptr;
    ASSERT_EQ(DDBGenerateContoursEx2(dem.string().c_str(), 1.0, 0, 0.0, nan, nan, 0.0, 1, nullptr, callback, &seen, &out),
              DDBERR_NONE);
    ASSERT_NE(out, nullptr);
    auto j = json::parse(std::string(out));
    DDBFree(out);
    EXPECT_FALSE(j["truncated"].get<bool>());
    EXPECT_EQ(j["featureCap"].get<int>(), 100000);
    EXPECT_GT(seen.calls, 3);
    EXPECT_DOUBLE_EQ(seen.last, 1.0);

    seen = Seen{};
    seen.stop = true;
    out = nullptr;
    EXPECT_EQ(DDBGenerateContoursEx2(dem.string().c_str(), 1.0, 0, 0.0, nan, nan, 0.0, 1, nullptr, callback, &seen, &out),
              DDBERR_CANCELED);
    EXPECT_EQ(out, nullptr);

    // No callback: the plain call
    ASSERT_EQ(DDBGenerateContoursEx2(dem.string().c_str(), 1.0, 0, 0.0, nan, nan, 0.0, 1, nullptr, nullptr, nullptr, &out),
              DDBERR_NONE);
    DDBFree(out);
}

} // namespace
