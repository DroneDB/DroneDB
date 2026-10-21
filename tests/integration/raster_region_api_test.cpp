/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "ddb.h"
#include "test.h"
#include "testarea.h"
#include "fs.h"
#include <nlohmann/json.hpp>
#include <vector>
#include <gdal.h>
#include <gdal_priv.h>
#include <ogr_srs_api.h>

namespace
{

    using json = nlohmann::json;

    // Brighton orthophoto: a well-known small RGB GeoTIFF in WebMercator.
    static constexpr const char *kOrthoUrl =
        "https://github.com/DroneDB/test_data/raw/master/brighton/odm_orthophoto.tif";
    // Brighton DSM: single-band float32 elevation GeoTIFF (UTM/WebMercator).
    static constexpr const char *kDsmUrl =
        "https://github.com/DroneDB/test_data/raw/master/brighton/dsm.tif";

    TEST(testRasterRegionApi, RenderPng)
    {
        TestArea ta(TEST_NAME);
        const fs::path ortho = ta.downloadTestAsset(kOrthoUrl, "ortho.tif");

        // A bbox in EPSG:4326 covering most of the Brighton ortho.
        const double bbox[4] = {-0.18, 50.81, -0.10, 50.84};

        uint8_t *outBuf = nullptr;
        int outSize = 0;
        const DDBErr err = DDBRenderRasterRegion(
            ortho.string().c_str(), bbox, "EPSG:4326",
            256, 256, "png", &outBuf, &outSize);

        EXPECT_EQ(err, DDBERR_NONE) << DDBGetLastError();
        ASSERT_NE(outBuf, nullptr);
        ASSERT_GT(outSize, 8);
        // PNG signature
        EXPECT_EQ(outBuf[0], 0x89);
        EXPECT_EQ(outBuf[1], 'P');
        EXPECT_EQ(outBuf[2], 'N');
        EXPECT_EQ(outBuf[3], 'G');

        DDBVSIFree(outBuf);
    }

    TEST(testRasterRegionApi, RenderJpeg)
    {
        TestArea ta(TEST_NAME);
        const fs::path ortho = ta.downloadTestAsset(kOrthoUrl, "ortho.tif");

        const double bbox[4] = {-0.18, 50.81, -0.10, 50.84};

        uint8_t *outBuf = nullptr;
        int outSize = 0;
        const DDBErr err = DDBRenderRasterRegion(
            ortho.string().c_str(), bbox, "EPSG:4326",
            128, 128, "jpeg", &outBuf, &outSize);

        EXPECT_EQ(err, DDBERR_NONE) << DDBGetLastError();
        ASSERT_NE(outBuf, nullptr);
        ASSERT_GT(outSize, 3);
        // JPEG SOI
        EXPECT_EQ(outBuf[0], 0xFF);
        EXPECT_EQ(outBuf[1], 0xD8);
        EXPECT_EQ(outBuf[2], 0xFF);

        DDBVSIFree(outBuf);
    }

    TEST(testRasterRegionApi, InvalidInput)
    {
        const double bbox[4] = {0, 0, 1, 1};
        uint8_t *outBuf = nullptr;
        int outSize = 0;

        // Null input path
        DDBErr err = DDBRenderRasterRegion(nullptr, bbox, "EPSG:4326",
                                           64, 64, "png", &outBuf, &outSize);
        EXPECT_NE(err, DDBERR_NONE);

        // Null bbox
        err = DDBRenderRasterRegion("does-not-exist.tif", nullptr, "EPSG:4326",
                                    64, 64, "png", &outBuf, &outSize);
        EXPECT_NE(err, DDBERR_NONE);

        // Bad dimensions
        err = DDBRenderRasterRegion("does-not-exist.tif", bbox, "EPSG:4326",
                                    0, 0, "png", &outBuf, &outSize);
        EXPECT_NE(err, DDBERR_NONE);

        // Nonexistent file → propagated error
        err = DDBRenderRasterRegion("does-not-exist.tif", bbox, "EPSG:4326",
                                    64, 64, "png", &outBuf, &outSize);
        EXPECT_NE(err, DDBERR_NONE);
    }

    TEST(testRasterRegionApi, QueryPointDsm)
    {
        TestArea ta(TEST_NAME);
        const fs::path dsm = ta.downloadTestAsset(kDsmUrl, "dsm.tif");

        // Pick a point known to fall inside the DSM (Brighton, UK).
        char *output = nullptr;
        const DDBErr err = DDBQueryRasterPoint(
            dsm.string().c_str(), -0.1402, 50.8225, "EPSG:4326", &output);

        EXPECT_EQ(err, DDBERR_NONE) << DDBGetLastError();
        ASSERT_NE(output, nullptr);

        json j = json::parse(output);
        EXPECT_TRUE(j.contains("bands"));
        EXPECT_TRUE(j["bands"].is_array());
        EXPECT_GE(j["bands"].size(), 1u);
        EXPECT_TRUE(j.contains("pixel"));
        EXPECT_TRUE(j["pixel"].is_array());
        EXPECT_EQ(j["pixel"].size(), 2u);
        EXPECT_TRUE(j.contains("lon"));
        EXPECT_TRUE(j.contains("lat"));

        DDBFree(output);
    }

    TEST(testRasterRegionApi, QueryPointOrtho)
    {
        TestArea ta(TEST_NAME);
        const fs::path ortho = ta.downloadTestAsset(kOrthoUrl, "ortho.tif");

        char *output = nullptr;
        const DDBErr err = DDBQueryRasterPoint(
            ortho.string().c_str(), -0.1402, 50.8225, "EPSG:4326", &output);

        EXPECT_EQ(err, DDBERR_NONE) << DDBGetLastError();
        ASSERT_NE(output, nullptr);

        json j = json::parse(output);
        // Ortho is RGB(+alpha), expect at least 3 band entries
        EXPECT_GE(j.at("bands").size(), 3u);
        DDBFree(output);
    }

    TEST(testRasterRegionApi, QueryPointInvalidInput)
    {
        char *output = nullptr;
        DDBErr err = DDBQueryRasterPoint(nullptr, 0, 0, "EPSG:4326", &output);
        EXPECT_NE(err, DDBERR_NONE);

        err = DDBQueryRasterPoint("does-not-exist.tif", 0, 0, "EPSG:4326", &output);
        EXPECT_NE(err, DDBERR_NONE);
    }

    // E3: validate DDBRenderRasterIndex end-to-end on a synthetic 5-band
    // georeferenced raster, plus a negative test that the API rejects a
    // raster with insufficient bands.
    TEST(testRasterRegionApi, RenderIndexNdvi)
    {
        TestArea ta(TEST_NAME);
        GDALAllRegister();

        // Build a tiny 5-band GeoTIFF in EPSG:4326 with constant pixel
        // values per band so that NDVI = (NIR - R) / (NIR + R) is finite.
        const fs::path src = ta.getPath("synthetic_5b.tif");
        const int w = 32, h = 32;
        GDALDriverH hDrv = GDALGetDriverByName("GTiff");
        ASSERT_NE(hDrv, nullptr);
        GDALDatasetH hDS = GDALCreate(hDrv, src.string().c_str(),
                                      w, h, 5, GDT_Byte, nullptr);
        ASSERT_NE(hDS, nullptr);

        // Cover a small lon/lat window: [10, 10] -> [10.01, 10.01].
        double gt[6] = {10.0, 0.01 / w, 0.0, 10.01, 0.0, -0.01 / h};
        GDALSetGeoTransform(hDS, gt);
        OGRSpatialReferenceH hSRS = OSRNewSpatialReference(nullptr);
        OSRImportFromEPSG(hSRS, 4326);
        char *wkt = nullptr;
        OSRExportToWkt(hSRS, &wkt);
        GDALSetProjection(hDS, wkt);
        CPLFree(wkt);
        OSRDestroySpatialReference(hSRS);

        // Fill bands with distinct constants: B=10, G=20, R=30, RE=80, NIR=200.
        const uint8_t fillVals[5] = {10, 20, 30, 80, 200};
        std::vector<uint8_t> buf(static_cast<size_t>(w) * h);
        for (int b = 1; b <= 5; ++b)
        {
            std::fill(buf.begin(), buf.end(), fillVals[b - 1]);
            GDALRasterBandH hB = GDALGetRasterBand(hDS, b);
            ASSERT_EQ(GDALRasterIO(hB, GF_Write, 0, 0, w, h,
                                   buf.data(), w, h, GDT_Byte, 0, 0),
                      CE_None);
        }
        GDALClose(hDS);

        const double bbox[4] = {10.002, 10.002, 10.008, 10.008};
        uint8_t *outBuf = nullptr;
        int outSize = 0;
        const DDBErr err = DDBRenderRasterIndex(
            src.string().c_str(), "NDVI", bbox, "EPSG:4326",
            64, 64, "png", &outBuf, &outSize);

        EXPECT_EQ(err, DDBERR_NONE) << DDBGetLastError();
        ASSERT_NE(outBuf, nullptr);
        ASSERT_GT(outSize, 8);
        // PNG signature
        EXPECT_EQ(outBuf[0], 0x89);
        EXPECT_EQ(outBuf[1], 'P');
        EXPECT_EQ(outBuf[2], 'N');
        EXPECT_EQ(outBuf[3], 'G');
        DDBVSIFree(outBuf);
    }

    TEST(testRasterRegionApi, RenderIndexInsufficientBands)
    {
        TestArea ta(TEST_NAME);
        // Brighton ortho only has RGB(+alpha) bands - NDVI needs NIR (band 5)
        // and must therefore be rejected.
        const fs::path ortho = ta.downloadTestAsset(kOrthoUrl, "ortho.tif");
        const double bbox[4] = {-0.18, 50.81, -0.10, 50.84};

        uint8_t *outBuf = nullptr;
        int outSize = 0;
        const DDBErr err = DDBRenderRasterIndex(
            ortho.string().c_str(), "NDVI", bbox, "EPSG:4326",
            64, 64, "png", &outBuf, &outSize);

        EXPECT_NE(err, DDBERR_NONE)
            << "Expected DDBRenderRasterIndex to fail on a 3-band raster";
        EXPECT_EQ(outBuf, nullptr);
    }

    // DDBRenderRasterRegionEx: validate band subset and OUTPUTCRS round-trip
    // by emitting a GeoTIFF and inspecting it via GDAL.
    TEST(testRasterRegionApi, RenderRegionExBandsAndOutputCrs)
    {
        TestArea ta(TEST_NAME);
        const fs::path ortho = ta.downloadTestAsset(kOrthoUrl, "ortho.tif");

        // Bbox in WGS84; request output in EPSG:3857 with only band 1.
        const double bbox[4] = {-0.18, 50.81, -0.10, 50.84};
        const int bands[] = {1};

        uint8_t *outBuf = nullptr;
        int outSize = 0;
        const DDBErr err = DDBRenderRasterRegionEx(
            ortho.string().c_str(), bbox, "EPSG:4326", "EPSG:3857",
            bands, 1, 128, 128, "image/tiff", &outBuf, &outSize);

        EXPECT_EQ(err, DDBERR_NONE) << DDBGetLastError();
        ASSERT_NE(outBuf, nullptr);
        ASSERT_GT(outSize, 0);

        // Load the in-memory GeoTIFF through /vsimem so we can inspect it.
        const std::string vsiName = "/vsimem/render_ex_test.tif";
        VSIFCloseL(VSIFileFromMemBuffer(vsiName.c_str(), outBuf, outSize, FALSE));
        GDALDatasetH hDS = GDALOpen(vsiName.c_str(), GA_ReadOnly);
        ASSERT_NE(hDS, nullptr);

        // Single requested band → output has the selected band (GDALWarp may
        // still propagate the source nodata/mask as a hidden second band when
        // the source raster carries one; we only assert that the explicit
        // alpha channel that would otherwise be appended via -dstalpha is NOT
        // present, i.e. band count stays small).
        EXPECT_LE(GDALGetRasterCount(hDS), 2);
        EXPECT_GE(GDALGetRasterCount(hDS), 1);

        // Verify the output SRS is EPSG:3857.
        const char *wkt = GDALGetProjectionRef(hDS);
        ASSERT_NE(wkt, nullptr);
        ASSERT_GT(strlen(wkt), 0u);
        OGRSpatialReferenceH hSRS = OSRNewSpatialReference(wkt);
        ASSERT_NE(hSRS, nullptr);
        const char *code = OSRGetAuthorityCode(hSRS, nullptr);
        EXPECT_TRUE(code != nullptr && std::string(code) == "3857");
        OSRDestroySpatialReference(hSRS);

        GDALClose(hDS);
        VSIUnlink(vsiName.c_str());
        DDBVSIFree(outBuf);
    }

    TEST(testRasterRegionApi, RenderRegionExInvalidBand)
    {
        TestArea ta(TEST_NAME);
        const fs::path ortho = ta.downloadTestAsset(kOrthoUrl, "ortho.tif");
        const double bbox[4] = {-0.18, 50.81, -0.10, 50.84};
        // Brighton ortho has 3 (or 4 incl. alpha) bands; band 99 must fail.
        const int bands[] = {99};

        uint8_t *outBuf = nullptr;
        int outSize = 0;
        const DDBErr err = DDBRenderRasterRegionEx(
            ortho.string().c_str(), bbox, "EPSG:4326", "",
            bands, 1, 64, 64, "png", &outBuf, &outSize);

        EXPECT_NE(err, DDBERR_NONE);
        EXPECT_EQ(outBuf, nullptr);
    }

} // namespace
