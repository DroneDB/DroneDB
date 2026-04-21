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

TEST(pointcloud, ptsInfo) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/refs/heads/master/point-clouds/test.pts",
        "test.pts");

    auto fp = ddb::fingerprint(pc);
    EXPECT_TRUE(fp == ddb::EntryType::PointCloud);

    ddb::PointCloudInfo ptsInfo;
    EXPECT_TRUE(ddb::getPointCloudInfo(pc.string(), ptsInfo));
    EXPECT_GT(ptsInfo.pointCount, 0) << "PTS file should have points";
    // PTS is text-based and lacks bounds in metadata (like PLY)
    EXPECT_TRUE(ptsInfo.bounds.empty()) << "PTS files should not have bounds in metadata";
}

TEST(pointcloud, xyzInfo) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/refs/heads/master/point-clouds/autzen-bmx-largersample.xyz",
        "autzen-bmx-largersample.xyz");

    auto fp = ddb::fingerprint(pc);
    EXPECT_TRUE(fp == ddb::EntryType::PointCloud);

    ddb::PointCloudInfo xyzInfo;
    EXPECT_TRUE(ddb::getPointCloudInfo(pc.string(), xyzInfo));
    EXPECT_GT(xyzInfo.pointCount, 0) << "XYZ file should have points";
    // XYZ is text-based and lacks bounds in metadata (like PLY)
    EXPECT_TRUE(xyzInfo.bounds.empty()) << "XYZ files should not have bounds in metadata";
}

TEST(pointcloud, eptFromPts) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/refs/heads/master/point-clouds/test.pts",
        "test.pts");

    auto fp = ddb::fingerprint(pc);
    EXPECT_TRUE(fp == ddb::EntryType::PointCloud);

    // Build EPT from PTS - this should work by converting to LAS first
    ddb::buildEpt({pc.string()}, ta.getFolder("ept").string());
    EXPECT_TRUE(fs::exists(ta.getFolder("ept") / "ept.json")) << "EPT generation from PTS should succeed";

    // Verify the generated EPT has valid bounds
    ddb::PointCloudInfo eptInfo;
    EXPECT_TRUE(ddb::getEptInfo((ta.getFolder("ept") / "ept.json").string(), eptInfo));
    EXPECT_EQ(eptInfo.bounds.size(), 6) << "EPT should have 6 bounds values";
    EXPECT_GT(eptInfo.pointCount, 0) << "EPT should have points";
}

TEST(pointcloud, eptFromXyz) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/refs/heads/master/point-clouds/autzen-bmx-largersample.xyz",
        "autzen-bmx-largersample.xyz");

    auto fp = ddb::fingerprint(pc);
    EXPECT_TRUE(fp == ddb::EntryType::PointCloud);

    // Build EPT from XYZ - this should work by converting to LAS first
    ddb::buildEpt({pc.string()}, ta.getFolder("ept").string());
    EXPECT_TRUE(fs::exists(ta.getFolder("ept") / "ept.json")) << "EPT generation from XYZ should succeed";

    // Verify the generated EPT has valid bounds
    ddb::PointCloudInfo eptInfo;
    EXPECT_TRUE(ddb::getEptInfo((ta.getFolder("ept") / "ept.json").string(), eptInfo));
    EXPECT_EQ(eptInfo.bounds.size(), 6) << "EPT should have 6 bounds values";
    EXPECT_GT(eptInfo.pointCount, 0) << "EPT should have points";
}

TEST(pointcloud, e57Info) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/refs/heads/master/point-clouds/A4.e57",
        "A4.e57");

    auto fp = ddb::fingerprint(pc);
    EXPECT_TRUE(fp == ddb::EntryType::PointCloud);

    ddb::PointCloudInfo e57Info;
    EXPECT_TRUE(ddb::getPointCloudInfo(pc.string(), e57Info));
    EXPECT_GT(e57Info.pointCount, 0) << "E57 file should have points";
}

TEST(pointcloud, eptFromE57) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/refs/heads/master/point-clouds/A4.e57",
        "A4.e57");

    auto fp = ddb::fingerprint(pc);
    EXPECT_TRUE(fp == ddb::EntryType::PointCloud);

    // Build EPT from E57 - this should work by converting to LAS first
    ddb::buildEpt({pc.string()}, ta.getFolder("ept").string());
    EXPECT_TRUE(fs::exists(ta.getFolder("ept") / "ept.json")) << "EPT generation from E57 should succeed";

    // Verify the generated EPT has valid bounds
    ddb::PointCloudInfo eptInfo;
    EXPECT_TRUE(ddb::getEptInfo((ta.getFolder("ept") / "ept.json").string(), eptInfo));
    EXPECT_EQ(eptInfo.bounds.size(), 6) << "EPT should have 6 bounds values";
    EXPECT_GT(eptInfo.pointCount, 0) << "EPT should have points";
}

}  // namespace
