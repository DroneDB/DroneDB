/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "ddb.h"
#include "test.h"
#include "testarea.h"
#include "fs.h"
#include <nlohmann/json.hpp>

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

} // namespace
