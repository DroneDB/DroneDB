/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"

#include "volume.h"
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

// Create a synthetic georeferenced Float32 DEM with a linear elevation gradient
// along the X axis: value(x,y) = x (in meters). Uses WGS84 so no CRS transformation needed.
fs::path createSyntheticWedgeDem(const fs::path &rasterPath, int w, int h,
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

    std::vector<float> row(static_cast<size_t>(w));
    GDALRasterBandH band = GDALGetRasterBand(hDs, 1);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) row[x] = static_cast<float>(x); // z = x
        GDALRasterIO(band, GF_Write, 0, y, w, 1,
                     row.data(), w, 1, GDT_Float32, 0, 0);
    }

    GDALSetRasterNoDataValue(band, -9999.0);
    GDALFlushCache(hDs);
    GDALClose(hDs);
    return rasterPath;
}

// Create a synthetic DEM with a parabolic mound: z = sqrt(x^2 + y^2)
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

    std::vector<float> row(static_cast<size_t>(w));
    GDALRasterBandH band = GDALGetRasterBand(hDs, 1);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            double dx = static_cast<double>(x) - static_cast<double>(w) / 2.0;
            double dy = static_cast<double>(y) - static_cast<double>(h) / 2.0;
            row[x] = std::sqrt(dx * dx + dy * dy); // parabolic mound
        }
        GDALRasterIO(band, GF_Write, 0, y, w, 1,
                     row.data(), w, 1, GDT_Float32, 0, 0);
    }

    GDALSetRasterNoDataValue(band, -9999.0);
    GDALFlushCache(hDs);
    GDALClose(hDs);
    return rasterPath;
}

// ---- Test 1: Flat-plane volume on a synthetic wedge -------------------------

TEST(rasterVolume, flatPlane) {
    TestArea ta(TEST_NAME);
    const int w = 50, h = 50;
    const double originLon = 10.0;
    const double originLat = 45.0;
    const double pix = 0.0001; // ~11 m at this latitude

    fs::path dem = createSyntheticWedgeDem(ta.getPath("dem_wedge.tif"),
                                            w, h, originLon, originLat, pix);

    // Square polygon covering the center of the raster
    const double midLat = originLat - (h / 2.0) * pix;
    const double halfW = 10.0 * pix; // 10 pixels wide in lon
    json poly;
    poly["type"] = "Polygon";
    poly["coordinates"] = json::array({
        json::array({
            json::array({originLon + halfW, midLat - halfW}),
            json::array({originLon + halfW, midLat + halfW}),
            json::array({originLon - halfW, midLat + halfW}),
            json::array({originLon - halfW, midLat - halfW}),
            json::array({originLon + halfW, midLat - halfW})
        })
    });

    // Use flat base method with elevation = 0 (below the wedge minimum)
    std::string out = calculateVolumeJson(dem.string(), poly.dump(), "flat", 0.0);
    auto j = json::parse(out);

    // Verify JSON schema
    EXPECT_TRUE(j.contains("cutVolume"));
    EXPECT_TRUE(j.contains("fillVolume"));
    EXPECT_TRUE(j.contains("netVolume"));
    EXPECT_TRUE(j.contains("area2d"));
    EXPECT_TRUE(j.contains("basePlaneMethod"));
    EXPECT_EQ(j["basePlaneMethod"].get<std::string>(), "flat");
    EXPECT_TRUE(j.contains("pixelSize"));
    EXPECT_TRUE(j.contains("crs"));
    EXPECT_TRUE(j.contains("boundaryPolygon"));
    EXPECT_TRUE(j.contains("pixelCount"));
    EXPECT_TRUE(j.contains("calculatedAt"));

    // Volumes should be non-negative (flat base at 0, wedge goes from 0 to w-1)
    EXPECT_GE(j["cutVolume"].get<double>(), 0.0);
    EXPECT_GE(j["fillVolume"].get<double>(), 0.0);
    EXPECT_GT(j["area2d"].get<double>(), 0.0);
}

// ---- Test 2: Lowest-perimeter base plane on a synthetic mound ----------------

TEST(rasterVolume, lowestPerimeterMound) {
    TestArea ta(TEST_NAME);
    const int w = 50, h = 50;
    const double originLon = 10.0;
    const double originLat = 45.0;
    const double pix = 0.0001;

    fs::path dem = createSyntheticMoundDem(ta.getPath("dem_mound.tif"),
                                            w, h, originLon, originLat, pix);

    // Polygon covering the mound center (20x20 pixels)
    const double midLat = originLat - (h / 2.0) * pix;
    const double halfW = 10.0 * pix;
    json poly;
    poly["type"] = "Polygon";
    poly["coordinates"] = json::array({
        json::array({
            json::array({originLon + halfW, midLat - halfW}),
            json::array({originLon + halfW, midLat + halfW}),
            json::array({originLon - halfW, midLat + halfW}),
            json::array({originLon - halfW, midLat - halfW}),
            json::array({originLon + halfW, midLat - halfW})
        })
    });

    std::string out = calculateVolumeJson(dem.string(), poly.dump(), "lowest_perimeter", 0.0);
    auto j = json::parse(out);

    // Verify JSON schema
    EXPECT_TRUE(j.contains("cutVolume"));
    EXPECT_TRUE(j.contains("fillVolume"));
    EXPECT_TRUE(j.contains("netVolume"));
    EXPECT_EQ(j["basePlaneMethod"].get<std::string>(), "lowest_perimeter");
    EXPECT_GT(j["area2d"].get<double>(), 0.0);

    // For a mound with lowest-perimeter, we expect fill volume > 0 (mound sits above base plane)
    EXPECT_GE(j["fillVolume"].get<double>(), 0.0);
}

// ---- Test 3: DDBCalculateVolume C-API smoke test -----------------------------

TEST(rasterVolume, cApiSmoke) {
    TestArea ta(TEST_NAME);
    const int w = 50, h = 50;
    const double originLon = 10.0;
    const double originLat = 45.0;
    const double pix = 0.0001;

    fs::path dem = createSyntheticWedgeDem(ta.getPath("dem_capi.tif"),
                                            w, h, originLon, originLat, pix);

    const double cLat = 45.0 - 25.0 * pix;
    json poly;
    poly["type"] = "Polygon";
    poly["coordinates"] = json::array({
        json::array({
            json::array({originLon + 0.001, cLat - 0.001}),
            json::array({originLon + 0.001, cLat + 0.001}),
            json::array({originLon - 0.001, cLat + 0.001}),
            json::array({originLon - 0.001, cLat - 0.001}),
            json::array({originLon + 0.001, cLat - 0.001})
        })
    });

    char *output = nullptr;
    auto err = DDBCalculateVolume(dem.string().c_str(),
                                   poly.dump().c_str(),
                                   "flat",
                                   0.0,
                                   &output);
    EXPECT_EQ(err, DDBERR_NONE);
    ASSERT_NE(output, nullptr);

    auto j = json::parse(std::string(output));
    EXPECT_TRUE(j.contains("cutVolume"));
    EXPECT_TRUE(j.contains("fillVolume"));
    EXPECT_TRUE(j.contains("netVolume"));
    EXPECT_TRUE(j.contains("area2d"));

    DDBFree(output);
}

// ---- Test 4: C-API invalid args ----------------------------------------------

TEST(rasterVolume, cApiInvalidArgs) {
    char *output = nullptr;
    EXPECT_NE(DDBCalculateVolume(nullptr, "{}", "flat", 0.0, &output), DDBERR_NONE);
    EXPECT_NE(DDBCalculateVolume("foo.tif", nullptr, "flat", 0.0, &output), DDBERR_NONE);
    EXPECT_NE(DDBCalculateVolume("foo.tif", "{}", nullptr, 0.0, &output), DDBERR_NONE);
}

} // namespace
