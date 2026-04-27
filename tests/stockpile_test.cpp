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

#include "json.h"

#include <cmath>
#include <cstdint>
#include <string>
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

// Create a synthetic DEM with a conical mound: z = max(0, R - sqrt(x^2 + y^2))
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

    const double R = std::min(static_cast<double>(w), static_cast<double>(h)) / 2.0;
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

    std::string out = detectStockpileJson(dem.string(),
                                           centerLat,
                                           centerLon,
                                           100.0, // 100m radius
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
                                   100.0, // radius in meters
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

} // namespace
