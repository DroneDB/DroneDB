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

TEST(pointcloud, copc) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/master/brighton/point_cloud.laz",
        "point_cloud.laz");

    ddb::buildCopc({pc.string()}, ta.getFolder("copc").string());
    EXPECT_TRUE(fs::exists(ta.getFolder("copc") / "cloud.copc.laz"));
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

TEST(pointcloud, copcFromPly) {
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
    ddb::buildCopc({pc.string()}, ta.getFolder("copc").string());
    EXPECT_TRUE(fs::exists(ta.getFolder("copc") / "cloud.copc.laz")) << "COPC generation from PLY should succeed";

    // Verify the generated EPT has valid bounds
    ddb::PointCloudInfo copcInfo;
    EXPECT_TRUE(ddb::getCopcInfo((ta.getFolder("copc") / "cloud.copc.laz").string(), copcInfo));
    EXPECT_EQ(copcInfo.bounds.size(), 6) << "COPC should have 6 bounds values";
    EXPECT_GT(copcInfo.pointCount, 0) << "COPC should have points";
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

TEST(pointcloud, copcFromPts) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/refs/heads/master/point-clouds/test.pts",
        "test.pts");

    auto fp = ddb::fingerprint(pc);
    EXPECT_TRUE(fp == ddb::EntryType::PointCloud);

    // Build EPT from PTS - this should work by converting to LAS first
    ddb::buildCopc({pc.string()}, ta.getFolder("copc").string());
    EXPECT_TRUE(fs::exists(ta.getFolder("copc") / "cloud.copc.laz")) << "COPC generation from PTS should succeed";

    // Verify the generated EPT has valid bounds
    ddb::PointCloudInfo copcInfo;
    EXPECT_TRUE(ddb::getCopcInfo((ta.getFolder("copc") / "cloud.copc.laz").string(), copcInfo));
    EXPECT_EQ(copcInfo.bounds.size(), 6) << "COPC should have 6 bounds values";
    EXPECT_GT(copcInfo.pointCount, 0) << "COPC should have points";
}

TEST(pointcloud, copcFromXyz) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/refs/heads/master/point-clouds/autzen-bmx-largersample.xyz",
        "autzen-bmx-largersample.xyz");

    auto fp = ddb::fingerprint(pc);
    EXPECT_TRUE(fp == ddb::EntryType::PointCloud);

    // Build EPT from XYZ - this should work by converting to LAS first
    ddb::buildCopc({pc.string()}, ta.getFolder("copc").string());
    EXPECT_TRUE(fs::exists(ta.getFolder("copc") / "cloud.copc.laz")) << "COPC generation from XYZ should succeed";

    // Verify the generated EPT has valid bounds
    ddb::PointCloudInfo copcInfo;
    EXPECT_TRUE(ddb::getCopcInfo((ta.getFolder("copc") / "cloud.copc.laz").string(), copcInfo));
    EXPECT_EQ(copcInfo.bounds.size(), 6) << "COPC should have 6 bounds values";
    EXPECT_GT(copcInfo.pointCount, 0) << "COPC should have points";
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

TEST(pointcloud, copcFromE57) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/refs/heads/master/point-clouds/A4.e57",
        "A4.e57");

    auto fp = ddb::fingerprint(pc);
    EXPECT_TRUE(fp == ddb::EntryType::PointCloud);

    // Build EPT from E57 - this should work by converting to LAS first
    ddb::buildCopc({pc.string()}, ta.getFolder("copc").string());
    EXPECT_TRUE(fs::exists(ta.getFolder("copc") / "cloud.copc.laz")) << "COPC generation from E57 should succeed";

    // Verify the generated EPT has valid bounds
    ddb::PointCloudInfo copcInfo;
    EXPECT_TRUE(ddb::getCopcInfo((ta.getFolder("copc") / "cloud.copc.laz").string(), copcInfo));
    EXPECT_EQ(copcInfo.bounds.size(), 6) << "COPC should have 6 bounds values";
    EXPECT_GT(copcInfo.pointCount, 0) << "COPC should have points";
}

TEST(pointcloud, xyzWithCloudCompareHeaders) {
    // Test file with //X Y Z R G B Return_Number Number_Of_Returns User_Data header
    TestArea ta(TEST_NAME);
    fs::path testFile = ta.getPath("cloudcompare_headers.xyz");
    std::ofstream ofs(testFile);
    ofs << "//X Y Z R G B Return_Number Number_Of_Returns User_Data\n";
    ofs << "274849.83 4603201.67 3.68 95 116 77 1.0 1.0 3.0\n";
    ofs << "274849.93 4603202.21 3.58 103 129 87 1.0 1.0 3.0\n";
    ofs.close();

    ddb::PointCloudInfo info;
    EXPECT_TRUE(ddb::getPointCloudInfo(testFile.string(), info));
    EXPECT_GT(info.pointCount, 0);
    EXPECT_TRUE(info.dimensions.size() >= 3); // At least X, Y, Z
}

TEST(pointcloud, xyzWithoutHeaders) {
    // Test file without header (space-separated)
    TestArea ta(TEST_NAME);
    fs::path testFile = ta.getPath("no_headers.xyz");
    std::ofstream ofs(testFile);
    ofs << "274849.83 4603201.67 3.68 95 116 77\n";
    ofs << "274849.93 4603202.21 3.58 103 129 87\n";
    ofs.close();

    ddb::PointCloudInfo info;
    EXPECT_TRUE(ddb::getPointCloudInfo(testFile.string(), info));
    EXPECT_EQ(info.pointCount, 2);
}

TEST(pointcloud, xyzWithPointCount) {
    // Test file with point count as first line
    TestArea ta(TEST_NAME);
    fs::path testFile = ta.getPath("with_count.xyz");
    std::ofstream ofs(testFile);
    ofs << "2\n";
    ofs << "274849.83 4603201.67 3.68 95 116 77\n";
    ofs << "274849.93 4603202.21 3.58 103 129 87\n";
    ofs.close();

    ddb::PointCloudInfo info;
    EXPECT_TRUE(ddb::getPointCloudInfo(testFile.string(), info));
    EXPECT_EQ(info.pointCount, 2);
}

TEST(pointcloud, xyzCommaSeparated) {
    // Test standard comma-separated format (existing test data)
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/master/point-clouds/utm17_1.xyz",
        "utm17_1.xyz");

    ddb::PointCloudInfo info;
    EXPECT_TRUE(ddb::getPointCloudInfo(pc.string(), info));
    EXPECT_GT(info.pointCount, 0);
}

TEST(pointcloud, xyzMultipleCommentLines) {
    // Test file with multiple comment lines at the start
    TestArea ta(TEST_NAME);
    fs::path testFile = ta.getPath("multiple_comments.xyz");
    std::ofstream ofs(testFile);
    ofs << "//Generated by CloudCompare\n";
    ofs << "//Export date: 2024-01-01\n";
    ofs << "//X Y Z R G B\n";
    ofs << "1.0 2.0 3.0 100 150 200\n";
    ofs << "4.0 5.0 6.0 110 160 210\n";
    ofs.close();

    ddb::PointCloudInfo info;
    EXPECT_TRUE(ddb::getPointCloudInfo(testFile.string(), info));
    EXPECT_EQ(info.pointCount, 2);
}

TEST(pointcloud, xyzTabSeparated) {
    // Test file with tab-separated values
    TestArea ta(TEST_NAME);
    fs::path testFile = ta.getPath("tab_separated.xyz");
    std::ofstream ofs(testFile);
    ofs << "X\tY\tZ\n";
    ofs << "1.0\t2.0\t3.0\n";
    ofs << "4.0\t5.0\t6.0\n";
    ofs.close();

    ddb::PointCloudInfo info;
    EXPECT_TRUE(ddb::getPointCloudInfo(testFile.string(), info));
    EXPECT_EQ(info.pointCount, 2);
}

TEST(pointcloud, xyzSemicolonSeparated) {
    // Test file with semicolon-separated values
    TestArea ta(TEST_NAME);
    fs::path testFile = ta.getPath("semicolon_separated.xyz");
    std::ofstream ofs(testFile);
    ofs << "X;Y;Z\n";
    ofs << "1.0;2.0;3.0\n";
    ofs << "4.0;5.0;6.0\n";
    ofs.close();

    ddb::PointCloudInfo info;
    EXPECT_TRUE(ddb::getPointCloudInfo(testFile.string(), info));
    EXPECT_EQ(info.pointCount, 2);
}

}  // namespace
