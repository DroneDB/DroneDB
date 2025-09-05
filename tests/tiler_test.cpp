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

    TEST(testTiler, toledoPointCloud)
    {

        TestArea ta(TEST_NAME);
        fs::path pc = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/refs/heads/master/point-clouds/toledo.laz",
                                           "point_cloud.laz");

        ddb::buildEpt({pc.string()}, ta.getFolder("ept").string());
        fs::path eptPath = ta.getPath(fs::path("ept") / "ept.json");

        // Test tiles from Toledo dataset coordinates
        struct ToledoTileTest {
            int z, x, y;
        };

        std::vector<ToledoTileTest> testTiles = {
            {18, 70123, 97753},
            {20, 280496, 391011},
            {22, 1121992, 1564041}
        };

        for (const auto& tile : testTiles) {
            fs::path outTile = TilerHelper::getTile(eptPath, tile.z, tile.x, tile.y, 256, true, true, ta.getFolder());

            EXPECT_TRUE(fs::exists(outTile)) << "Tile " << tile.z << "/" << tile.x << "/" << tile.y << " not found";
            EXPECT_GT(io::Path(outTile).getSize(), 0) << "Tile " << tile.z << "/" << tile.x << "/" << tile.y << " is empty";
        }

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

    TEST(testTiler, MultipleZoomLevels)
    {
        TestArea ta(TEST_NAME);
        fs::path ortho = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/refs/heads/master/ortho/wro.tif",
                                              "wro.tif");
        fs::path tileDir = ta.getFolder("tiles");

        GDALTiler t(ortho.string(), tileDir.string(), 256, true);

        // Test tiles from the provided table
        struct TileTest {
            int z, x, y;
            int tileSize;
        };

        std::vector<TileTest> testTiles = {
            {14, 16174, 10245, 256},
            {18, 258796, 163923, 256},
            {18, 258797, 163923, 256},
            {18, 258796, 163922, 256},
            {18, 258797, 163922, 256},
            {19, 517593, 327846, 256},
            {20, 1035186, 655693, 256},
            {20, 1035187, 655693, 256},
            {20, 1035186, 655694, 256}
        };

        for (const auto& tile : testTiles) {
            t.tile(tile.z, tile.x, tile.y);

            fs::path expectedTile = tileDir / std::to_string(tile.z) / std::to_string(tile.x) /
                                   (std::to_string(tile.y) + ".png");

            EXPECT_TRUE(fs::exists(expectedTile)) << "Tile " << tile.z << "/" << tile.x << "/" << tile.y << " not found";
        }
    }

}
