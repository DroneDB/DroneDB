/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"

#include "raster_profile.h"
#include "ddb.h"
#include "gdal_inc.h"
#include "exceptions.h"

#include "json.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <limits>

namespace {

using namespace ddb;

// Create a synthetic georeferenced Float32 DEM with a deterministic gradient
// along the X axis: value(x,y) = x (in pixels). Geographic CRS EPSG:4326
// is used so we can drive the profile function with WGS84 coordinates that
// require no coordinate transformation.
fs::path createSyntheticDem(const fs::path &rasterPath, int w, int h,
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

    // North-up geotransform: pixel row 0 is at the top (north).
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

// ---- Happy path: gradient-only DEM -----------------------------------------

TEST(rasterProfile, happyPathGradient) {
    TestArea ta(TEST_NAME);
    const int w = 100, h = 50;
    const double originLon = 10.0;
    const double originLat = 45.0;
    const double pix = 0.0001; // ~11 m at this latitude

    fs::path dem = createSyntheticDem(ta.getPath("dem_gradient.tif"),
                                      w, h, originLon, originLat, pix);

    // Horizontal line across the middle row, from left edge to right edge.
    const double midLat = originLat - (h / 2.0) * pix;
    const double lon0 = originLon + 0.5 * pix;           // first pixel center
    const double lon1 = originLon + (w - 0.5) * pix;     // last pixel center

    json ls = {
        {"type", "LineString"},
        {"coordinates", {{lon0, midLat}, {lon1, midLat}}}
    };

    std::string out = getRasterProfileJson(dem.string(), ls.dump(), 20);
    auto j = json::parse(out);

    EXPECT_TRUE(j.contains("samples"));
    EXPECT_EQ(j["sampleCount"].get<int>(), 20);
    EXPECT_EQ(j["validCount"].get<int>(), 20);

    // Values should monotonically increase along the gradient (values == column index).
    double prev = -std::numeric_limits<double>::infinity();
    for (auto &s : j["samples"]) {
        ASSERT_FALSE(s["value"].is_null());
        double v = s["value"].get<double>();
        EXPECT_GE(v, prev - 1e-3);
        prev = v;
    }

    // First and last values should be close to 0 and w-1 respectively.
    EXPECT_NEAR(j["samples"].front()["value"].get<double>(), 0.0, 1.0);
    EXPECT_NEAR(j["samples"].back()["value"].get<double>(),
                static_cast<double>(w - 1), 1.0);

    EXPECT_GT(j["totalLength"].get<double>(), 0.0);
    EXPECT_EQ(j["unit"].get<std::string>(), "m");
    // Note: isThermal may be true for synthetic rasters whose value range
    // overlaps with ambient temperatures (triggering the direct-temperature
    // heuristic). That's fine and unrelated to the profile correctness.
}

// ---- Samples clamping ------------------------------------------------------

TEST(rasterProfile, samplesClamping) {
    TestArea ta(TEST_NAME);
    fs::path dem = createSyntheticDem(ta.getPath("dem_clamp.tif"),
                                      16, 16, 0.0, 0.0, 0.0001);

    json ls = {
        {"type", "LineString"},
        {"coordinates", {{0.0001, -0.0001}, {0.0010, -0.0010}}}
    };

    // samples < 2 should fall back to a sane default (>=2).
    auto j1 = json::parse(getRasterProfileJson(dem.string(), ls.dump(), 0));
    EXPECT_GE(j1["sampleCount"].get<int>(), 2);

    // Huge values should be clamped to MAX_SAMPLES (4096).
    auto j2 = json::parse(getRasterProfileJson(dem.string(), ls.dump(), 999999));
    EXPECT_LE(j2["sampleCount"].get<int>(), 4096);
}

// ---- Nodata handling -------------------------------------------------------

TEST(rasterProfile, nodataProducesNullValue) {
    TestArea ta(TEST_NAME);
    const int w = 20, h = 20;
    const double originLon = 5.0, originLat = 5.0, pix = 0.0001;
    fs::path dem = createSyntheticDem(ta.getPath("dem_nodata.tif"),
                                      w, h, originLon, originLat, pix);

    // Set nodata and plant nodata cells on a central row.
    GDALDatasetH hDs = GDALOpen(dem.string().c_str(), GA_Update);
    ASSERT_NE(hDs, nullptr);
    GDALRasterBandH band = GDALGetRasterBand(hDs, 1);
    GDALSetRasterNoDataValue(band, -9999);
    std::vector<float> row(w, -9999.0f);
    GDALRasterIO(band, GF_Write, 0, h / 2, w, 1,
                 row.data(), w, 1, GDT_Float32, 0, 0);
    GDALFlushCache(hDs);
    GDALClose(hDs);

    // Run a vertical profile through the corrupted row.
    const double lon = originLon + (w / 2.0) * pix;
    const double latTop = originLat - 0.5 * pix;
    const double latBot = originLat - (h - 0.5) * pix;

    json ls = {
        {"type", "LineString"},
        {"coordinates", {{lon, latTop}, {lon, latBot}}}
    };

    auto j = json::parse(getRasterProfileJson(dem.string(), ls.dump(), 64));
    int nullCount = 0;
    for (auto &s : j["samples"])
        if (s["value"].is_null()) nullCount++;
    EXPECT_GT(nullCount, 0);
    EXPECT_LT(nullCount, j["sampleCount"].get<int>());
}

// ---- Degenerate geometries -------------------------------------------------

TEST(rasterProfile, emptyLineStringThrows) {
    TestArea ta(TEST_NAME);
    fs::path dem = createSyntheticDem(ta.getPath("dem_empty.tif"),
                                      10, 10, 0.0, 0.0, 0.0001);

    json badShape = {
        {"type", "Point"},
        {"coordinates", {0.0, 0.0}}
    };
    EXPECT_THROW(getRasterProfileJson(dem.string(), badShape.dump(), 10),
                 InvalidArgsException);

    json singlePoint = {
        {"type", "LineString"},
        {"coordinates", {{0.0, 0.0}}}
    };
    EXPECT_THROW(getRasterProfileJson(dem.string(), singlePoint.dump(), 10),
                 InvalidArgsException);

    json coincident = {
        {"type", "LineString"},
        {"coordinates", {{0.0, 0.0}, {0.0, 0.0}}}
    };
    EXPECT_THROW(getRasterProfileJson(dem.string(), coincident.dump(), 10),
                 InvalidArgsException);
}

TEST(rasterProfile, invalidGeoJsonThrows) {
    TestArea ta(TEST_NAME);
    fs::path dem = createSyntheticDem(ta.getPath("dem_invalid_gj.tif"),
                                      10, 10, 0.0, 0.0, 0.0001);
    EXPECT_THROW(getRasterProfileJson(dem.string(), "{not-json}", 10),
                 InvalidArgsException);
}

TEST(rasterProfile, missingRasterThrows) {
    EXPECT_THROW(
        getRasterProfileJson("nonexistent_raster_path.tif",
            R"({"type":"LineString","coordinates":[[0,0],[1,1]]})", 10),
        AppException);
}

// ---- C API round-trip ------------------------------------------------------

TEST(rasterProfile, cApiGetRasterProfile) {
    TestArea ta(TEST_NAME);
    fs::path dem = createSyntheticDem(ta.getPath("dem_capi.tif"),
                                      32, 32, 0.0, 0.0, 0.0001);

    json ls = {
        {"type", "LineString"},
        {"coordinates", {{0.00005, -0.00005}, {0.0030, -0.00005}}}
    };
    std::string lsStr = ls.dump();

    char *output = nullptr;
    auto err = DDBGetRasterProfile(dem.string().c_str(), lsStr.c_str(), 16, &output);
    EXPECT_EQ(err, DDBERR_NONE);
    ASSERT_NE(output, nullptr);

    auto j = json::parse(std::string(output));
    EXPECT_EQ(j["sampleCount"].get<int>(), 16);
    EXPECT_TRUE(j.contains("totalLength"));

    DDBFree(output);
}

TEST(rasterProfile, cApiInvalidArgs) {
    char *output = nullptr;
    EXPECT_NE(DDBGetRasterProfile(nullptr, "{}", 10, &output), DDBERR_NONE);
    EXPECT_NE(DDBGetRasterProfile("foo.tif", nullptr, 10, &output), DDBERR_NONE);
    EXPECT_NE(DDBGetRasterProfile("foo.tif", "{}", 10, nullptr), DDBERR_NONE);
}

} // namespace
