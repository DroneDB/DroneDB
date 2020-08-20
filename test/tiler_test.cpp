/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "tiler.h"

namespace {

using namespace ddb;

TEST(testTiler, RGB) {
    Tiler t("/data/drone/brighton2/odm_orthophoto/odm_orthophoto.tif", "/data/drone/brighton2/tiles/");
    t.tile(19, 128168, 339545);
}

TEST(testTiler, DSM){
    Tiler t("/data/drone/brighton2/odm_dem/dsm.tif", "/data/drone/brighton2/dsm_tiles/");
    t.tile(21, 512674, 1358189);
    t.tile(20, 256337, 679094);
}

TEST(testTiler, TMS){
    Tiler tms("/data/drone/brighton2/odm_dem/dsm.tif", "/data/drone/brighton2/dsm_tiles/", 256, true);
    tms.tile(20, 256337, 369481);
}

}
