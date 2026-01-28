/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "pointcloud.h"
#include "3d.h"

#include "dbops.h"
#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"

namespace {

using namespace ddb;

TEST(pointcloud, parse) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/master/brighton/point_cloud.laz",
        "point_cloud.laz");

    auto fp = ddb::fingerprint(pc);
    EXPECT_TRUE(fp == ddb::EntryType::PointCloud);

    ddb::PointCloudInfo i;
    EXPECT_TRUE(ddb::getPointCloudInfo(pc.string(), i));
    EXPECT_EQ(i.pointCount, 24503);
}

TEST(pointcloud, ept) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/master/brighton/point_cloud.laz",
        "point_cloud.laz");

    ddb::buildEpt({pc.string()}, ta.getFolder("ept").string());
    EXPECT_TRUE(fs::exists(ta.getFolder("ept") / "ept.json"));
}

TEST(pointcloud, toledoInfo) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/refs/heads/master/point-clouds/toledo.laz",
        "point_cloud.laz");

    auto fp = ddb::fingerprint(pc);
    EXPECT_TRUE(fp == ddb::EntryType::PointCloud);

    ddb::PointCloudInfo i;
    EXPECT_TRUE(ddb::getPointCloudInfo(pc.string(), i));

    LOGD << "(" << i.bounds[0] << ", " << i.bounds[1] << ", " << i.bounds[2] << "); (" << i.bounds[3] << ", " << i.bounds[4] << ", " << i.bounds[5] << ")";

    LOGD << i.centroid.toGeoJSON().dump(4);
}

TEST(pointcloud, eptFromPly) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/refs/heads/master/point-clouds/point_cloud.ply",
        "point_cloud.ply");

    auto fp = ddb::fingerprint(pc);
    EXPECT_TRUE(fp == ddb::EntryType::PointCloud);

    // Verify PLY info doesn't have bounds (this is expected)
    ddb::PointCloudInfo plyInfo;
    EXPECT_TRUE(ddb::getPointCloudInfo(pc.string(), plyInfo));
    EXPECT_TRUE(plyInfo.bounds.empty()) << "PLY files should not have bounds in metadata";
    EXPECT_GT(plyInfo.pointCount, 0) << "PLY file should have points";

    // Build EPT from PLY - this should work by reading bounds from converted LAS
    ddb::buildEpt({pc.string()}, ta.getFolder("ept").string());
    EXPECT_TRUE(fs::exists(ta.getFolder("ept") / "ept.json")) << "EPT generation from PLY should succeed";

    // Verify the generated EPT has valid bounds
    ddb::PointCloudInfo eptInfo;
    EXPECT_TRUE(ddb::getEptInfo((ta.getFolder("ept") / "ept.json").string(), eptInfo));
    EXPECT_EQ(eptInfo.bounds.size(), 6) << "EPT should have 6 bounds values";
    EXPECT_GT(eptInfo.pointCount, 0) << "EPT should have points";
}

TEST(pointcloud, nexusFromPlyMesh) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/refs/heads/master/point-clouds/odm_25dmesh.ply",
        "odm_25dmesh.ply");

    // Verify the file is correctly identified as a Model (mesh), not PointCloud
    auto fp = ddb::fingerprint(pc);
    EXPECT_TRUE(fp == ddb::EntryType::Model);

    // Convert PLY mesh to Nexus
    fs::path nexusOutput = ta.getPath("odm_25dmesh.nxz");
    std::string nexusPath = buildNexus(pc.string(), nexusOutput.string(), true);

    // Verify nexus file was created
    ASSERT_FALSE(nexusPath.empty());
    ASSERT_TRUE(fs::exists(nexusPath));
    ASSERT_GT(fs::file_size(nexusPath), 0);

    LOGD << "Successfully created nexus file: " << nexusPath;
    LOGD << "Nexus file size: " << fs::file_size(nexusPath) << " bytes";

}

}  // namespace
