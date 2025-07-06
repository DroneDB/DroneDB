/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "test.h"
#include "gdaltiler.h"
#include "testarea.h"
#include "tilerhelper.h"
#include "mio.h"
#include "pointcloud.h"
#include "ddb.h"

namespace
{

    using namespace ddb;

    TEST(testTiler, RGB)
    {
        TestArea ta(TEST_NAME);
        fs::path ortho = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/brighton/odm_orthophoto.tif",
                                              "ortho.tif");
        fs::path tileDir = ta.getFolder("tiles");

        GDALTiler t(ortho.string(), tileDir.string());

        t.tile(19, 128168, 339545);

        EXPECT_TRUE(fs::exists(tileDir / "19" / "128168" / "339545.png"));
    }

    TEST(testTiler, DSM)
    {
        TestArea ta(TEST_NAME);
        fs::path dsm = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/brighton/dsm.tif",
                                            "dsm.tif");
        fs::path tileDir = ta.getFolder("tiles");

        GDALTiler t(dsm.string(), tileDir.string());
        t.tile(21, 512674, 1358189);
        t.tile(20, 256337, 679094);

        EXPECT_TRUE(fs::exists(tileDir / "21" / "512674" / "1358189.png"));
        EXPECT_TRUE(fs::exists(tileDir / "20" / "256337" / "679094.png"));

        fs::path tmsTileDir = ta.getFolder("tmsTiles");
        GDALTiler tms(dsm.string(), tmsTileDir.string(), 256, true);
        tms.tile(20, 256337, 369481);

        EXPECT_TRUE(fs::exists(tmsTileDir / "20" / "256337" / "369481.png"));

        // TODO: more tests
        //      - edge cases
        //      - out of bounds
        //      - different tile sizes
    }

    TEST(testTiler, image)
    {

        TestArea ta(TEST_NAME);
        fs::path pc = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/brighton/point_cloud.laz",
                                           "point_cloud.laz");

        ddb::buildEpt({pc.string()}, ta.getFolder("ept").string());
        fs::path eptPath = ta.getPath(fs::path("ept") / "ept.json");

        fs::path outTile = TilerHelper::getTile(eptPath, 20, 256337, 369481, 256, true, true, ta.getFolder());

        EXPECT_TRUE(fs::exists(outTile));

        /* I'm disabling this test because it is flaky on some systems
        uint8_t *buffer;
        int bufSize;
        TilerHelper::getTile(eptPath, 20, 256337, 369481, 256, true, true, "", &buffer, &bufSize);

        EXPECT_TRUE(bufSize > 0);
        EXPECT_EQ(io::Path(outTile).getSize(), bufSize);

        fs::path outMemoryTile = ta.getPath("output-memory.png");

        std::ofstream of(outMemoryTile.string(), std::ios::out | std::ios::binary | std::ios::trunc);
        of.write(reinterpret_cast<char *>(buffer), bufSize);
        of.close();

        #ifndef _WIN32

        EXPECT_EQ(Hash::fileSHA256(outMemoryTile.string()),
                  Hash::fileSHA256(outTile.string()));

        #endif

        DDBVSIFree(buffer);
        */
    }

    TEST(testTiler, userCache)
    {
        TestArea ta(TEST_NAME);
        fs::path image = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/test-datasets/drone_dataset_brighton_beach/DJI_0018.JPG",
                                              "DJI_0032.JPG");
        fs::path tileDir = ta.getFolder("tiles");
        fs::path tile = TilerHelper::getFromUserCache(image, 20, 256335, 369483, 512, true, true, "");

        EXPECT_TRUE(io::Path(tile).getSize() > 0);
    }

}
