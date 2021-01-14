/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "test.h"
#include "tiler.h"
#include "testarea.h"

namespace {

using namespace ddb;

TEST(testTiler, RGB) {
    TestArea ta(TEST_NAME);
    fs::path ortho = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/brighton/odm_orthophoto.tif",
                                          "ortho.tif");
    fs::path tileDir = ta.getFolder("tiles");
    
    Tiler t(ortho.string(), tileDir.string());
    
    t.tile(19, 128168, 339545);
    
    EXPECT_TRUE(fs::exists(tileDir / "19" / "128168" / "339545.png"));

}

TEST(testTiler, DSM){
    TestArea ta(TEST_NAME);
    fs::path dsm = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/brighton/dsm.tif",
                                          "dsm.tif");
    fs::path tileDir = ta.getFolder("tiles");

    Tiler t(dsm.string(), tileDir.string());
    t.tile(21, 512674, 1358189);
    t.tile(20, 256337, 679094);

    EXPECT_TRUE(fs::exists(tileDir / "21" / "512674" / "1358189.png"));
    EXPECT_TRUE(fs::exists(tileDir / "20" / "256337" / "679094.png"));

    fs::path tmsTileDir = ta.getFolder("tmsTiles");
    Tiler tms(dsm.string(), tmsTileDir.string(), 256, true);
    tms.tile(20, 256337, 369481);

    EXPECT_TRUE(fs::exists(tmsTileDir / "20" / "256337" / "369481.png"));

    // TODO: more tests
    //      - edge cases
    //      - out of bounds
    //      - different tile sizes
}

}
