/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "pointcloud.h"
#include "dbops.h"
#include "test.h"
#include "testarea.h"

namespace {

using namespace ddb;

TEST(pointcloud, parse) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/brighton/point_cloud.laz",
                                          "point_cloud.laz");

    auto fp = ddb::fingerprint(pc);
    EXPECT_TRUE(fp == ddb::EntryType::PointCloud);

    ddb::PointCloudInfo i;
    EXPECT_TRUE(ddb::getPointCloudInfo(pc.string(), i));
    EXPECT_EQ(i.pointCount, 24503);
}

TEST(pointcloud, ept){
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/brighton/point_cloud.laz",
                                          "point_cloud.laz");

    ddb::buildEpt({ pc.string() }, ta.getFolder("ept").string());
    EXPECT_TRUE(fs::exists(ta.getFolder("ept") / "ept.json"));
}


}
