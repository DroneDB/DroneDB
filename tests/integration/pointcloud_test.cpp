/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "pointcloud.h"
#include "3d.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "build_budget.h"
#include "ddbt/ddbt.h"
#include "dbops.h"
#include "exceptions.h"
#include "gtest/gtest.h"
#include "las_test_writer.h"
#include "mio.h"
#include "test.h"
#include "testarea.h"
#include "testfs.h"
#include "thumbs.h"
#include "utils.h"


namespace {

// The count and the XYZ bounds of the points of a text cloud (XYZ, PTS): its lines that start with at
// least three numbers, whatever separates them (a header, a point count or a comment is no point)
struct TextCloud {
    size_t count = 0;
    double min[3] = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                     std::numeric_limits<double>::max()};
    double max[3] = {std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(),
                     std::numeric_limits<double>::lowest()};
};

TextCloud readTextCloud(const fs::path& file) {
    TextCloud cloud;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        std::replace_if(line.begin(), line.end(), [](char c) { return c == ',' || c == ';' || c == '\t'; }, ' ');
        std::istringstream tokens(line);
        std::string token;
        std::vector<double> values;
        while (values.size() < 3 && tokens >> token) {
            char* end = nullptr;
            const double value = std::strtod(token.c_str(), &end);
            if (end == token.c_str() || *end != '\0')
                break;
            values.push_back(value);
        }
        if (values.size() < 3)
            continue;
        cloud.count++;
        for (size_t a = 0; a < 3; a++) {
            cloud.min[a] = std::min(cloud.min[a], values[a]);
            cloud.max[a] = std::max(cloud.max[a], values[a]);
        }
    }
    return cloud;
}

// Builds the COPC of `sources` in `folder` and reads it back
ddb::PointCloudInfo buildAndRead(const std::vector<std::string>& sources, const fs::path& folder) {
    ddb::buildCopc(sources, folder.string());
    ddb::PointCloudInfo copc;
    EXPECT_TRUE(ddb::getCopcInfo((folder / "cloud.copc.laz").string(), copc));
    EXPECT_EQ(copc.bounds.size(), 6u);
    return copc;
}

// The COPC holds the points of the text cloud: as many, within the bounds of their coordinates (the
// COPC stores them on a grid of `tolerance`)
void expectPointsOf(const TextCloud& source, const ddb::PointCloudInfo& copc, double tolerance) {
    ASSERT_GT(source.count, 0u);
    EXPECT_EQ(copc.pointCount, source.count);
    ASSERT_EQ(copc.bounds.size(), 6u);
    for (size_t a = 0; a < 3; a++) {
        EXPECT_NEAR(copc.bounds[a], source.min[a], tolerance) << "min of axis " << a;
        EXPECT_NEAR(copc.bounds[3 + a], source.max[a], tolerance) << "max of axis " << a;
    }
}

// The COPC holds the points of a cloud whose header gives its count and its bounds (LAS, LAZ)
void expectPointsOf(const ddb::PointCloudInfo& source, const ddb::PointCloudInfo& copc, size_t copies = 1) {
    EXPECT_EQ(copc.pointCount, source.pointCount * copies);
    ASSERT_EQ(source.bounds.size(), 6u);
    ASSERT_EQ(copc.bounds.size(), 6u);
    for (size_t i = 0; i < 6; i++)
        EXPECT_NEAR(copc.bounds[i], source.bounds[i], 1e-6) << "bound " << i;
}

// Nothing of a build that failed is left in its folder: no COPC, no private folder
void expectNothingIn(const fs::path& folder) {
    EXPECT_FALSE(fs::exists(folder / "cloud.copc.laz"));
    for (const auto& entry : fs::directory_iterator(folder))
        ADD_FAILURE() << "left behind: " << entry.path().filename().string();
}

using namespace ddb;

TEST(pointcloud, parse) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        testDataUrl("brighton/point_cloud.laz"),
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
        testDataUrl("brighton/point_cloud.laz"),
        "point_cloud.laz");

    ddb::PointCloudInfo source;
    ASSERT_TRUE(ddb::getPointCloudInfo(pc.string(), source));
    expectPointsOf(source, buildAndRead({pc.string()}, ta.getFolder("copc")));
}

TEST(pointcloud, toledoInfo) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        testDataUrl("point-clouds/toledo.laz"),
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
        testDataUrl("point-clouds/point_cloud.ply"),
        "point_cloud.ply");

    auto fp = ddb::fingerprint(pc);
    EXPECT_TRUE(fp == ddb::EntryType::PointCloud);

    // PLY info has no bounds: the build does not need them
    ddb::PointCloudInfo plyInfo;
    EXPECT_TRUE(ddb::getPointCloudInfo(pc.string(), plyInfo));
    EXPECT_TRUE(plyInfo.bounds.empty()) << "PLY files should not have bounds in metadata";
    EXPECT_GT(plyInfo.pointCount, 0) << "PLY file should have points";

    const ddb::PointCloudInfo copc = buildAndRead({pc.string()}, ta.getFolder("copc"));
    EXPECT_EQ(copc.pointCount, plyInfo.pointCount);
}

TEST(pointcloud, bm3dFromPlyMesh) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        testDataUrl("point-clouds/odm_25dmesh.ply"),
        "odm_25dmesh.ply");

    // Verify the file is correctly identified as a Model (mesh), not PointCloud
    auto fp = ddb::fingerprint(pc);
    EXPECT_TRUE(fp == ddb::EntryType::Model);

    // Convert PLY mesh to bm3d
    fs::path bm3dOutput = ta.getPath("odm_25dmesh.bm3d");
    std::string bm3dPath = buildBm3d(pc.string(), bm3dOutput.string(), true);

    // Verify the bm3d file was created
    ASSERT_FALSE(bm3dPath.empty());
    ASSERT_TRUE(fs::exists(bm3dPath));
    ASSERT_GT(fs::file_size(bm3dPath), 0);

    LOGD << "Successfully created bm3d file: " << bm3dPath;
    LOGD << "bm3d file size: " << fs::file_size(bm3dPath) << " bytes";
}

TEST(pointcloud, ptsInfo) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        testDataUrl("point-clouds/test.pts"),
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
        testDataUrl("point-clouds/autzen-bmx-largersample.xyz"),
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
        testDataUrl("point-clouds/test.pts"),
        "test.pts");

    auto fp = ddb::fingerprint(pc);
    EXPECT_TRUE(fp == ddb::EntryType::PointCloud);

    expectPointsOf(readTextCloud(pc), buildAndRead({pc.string()}, ta.getFolder("copc")), 1e-3);
}

TEST(pointcloud, copcFromXyz) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        testDataUrl("point-clouds/autzen-bmx-largersample.xyz"),
        "autzen-bmx-largersample.xyz");

    auto fp = ddb::fingerprint(pc);
    EXPECT_TRUE(fp == ddb::EntryType::PointCloud);

    expectPointsOf(readTextCloud(pc), buildAndRead({pc.string()}, ta.getFolder("copc")), 1e-3);
}

TEST(pointcloud, e57Info) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        testDataUrl("point-clouds/A4.e57"),
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
        testDataUrl("point-clouds/A4.e57"),
        "A4.e57");

    auto fp = ddb::fingerprint(pc);
    EXPECT_TRUE(fp == ddb::EntryType::PointCloud);

    ddb::PointCloudInfo e57Info;
    ASSERT_TRUE(ddb::getPointCloudInfo(pc.string(), e57Info));
    const ddb::PointCloudInfo copc = buildAndRead({pc.string()}, ta.getFolder("copc"));
    EXPECT_EQ(copc.pointCount, e57Info.pointCount);
    EXPECT_GT(copc.bounds[3], copc.bounds[0]);
    EXPECT_GT(copc.bounds[4], copc.bounds[1]);
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
        testDataUrl("point-clouds/utm17_1.xyz"),
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

// ---------------------------------------------------------------------------
// COPC builds (ddb-tilers, in the raster helper)
// ---------------------------------------------------------------------------

// The text clouds DroneDB indexes (getPointCloudInfo reads them above) build with all their points:
// a CloudCompare header, a point count line, comments, tabs, semicolons and commas
TEST(pointcloud, copcOfEveryTextLayoutHasAllItsPoints) {
    TestArea ta(TEST_NAME);
    const std::vector<std::pair<std::string, std::string>> clouds = {
        {"cloudcompare_headers.xyz",
         "//X Y Z R G B Return_Number Number_Of_Returns User_Data\n"
         "274849.83 4603201.67 3.68 95 116 77 1.0 1.0 3.0\n"
         "274849.93 4603202.21 3.58 103 129 87 1.0 1.0 3.0\n"
         "274850.13 4603202.71 3.48 104 130 88 1.0 1.0 3.0\n"},
        {"no_headers.xyz", "274849.83 4603201.67 3.68 95 116 77\n274849.93 4603202.21 3.58 103 129 87\n"},
        {"with_count.xyz", "2\n274849.83 4603201.67 3.68 95 116 77\n274849.93 4603202.21 3.58 103 129 87\n"},
        {"multiple_comments.xyz",
         "//Generated by CloudCompare\n//Export date: 2024-01-01\n//X Y Z R G B\n"
         "1.0 2.0 3.0 100 150 200\n4.0 5.0 6.0 110 160 210\n"},
        {"tab_separated.xyz", "X\tY\tZ\n1.0\t2.0\t3.0\n4.0\t5.0\t6.0\n"},
        {"semicolon_separated.xyz", "X;Y;Z\n1.0;2.0;3.0\n4.0;5.0;6.0\n"},
        {"comma_separated.xyz", "X,Y,Z\n289814.15,4320978.61,170.76\n289814.64,4320978.84,170.75\n"}};
    for (const auto& [name, text] : clouds) {
        SCOPED_TRACE(name);
        const fs::path file = ta.getPath(name);
        std::ofstream(file) << text;
        const fs::path out = ta.getFolder("copc-" + fs::path(name).stem().string());
        expectPointsOf(readTextCloud(file), buildAndRead({file.string()}, out), 1e-3);
    }
}

// The clouds of a build are merged into one, each read where it lies (the helper reads the folder of
// each of them)
TEST(pointcloud, copcOfSeveralCloudsHasThePointsOfAll) {
    TestArea ta(TEST_NAME);
    const fs::path pc = ta.downloadTestAsset(testDataUrl("brighton/point_cloud.laz"), "point_cloud.laz");
    const fs::path first = ta.getFolder("a") / "first.laz";
    const fs::path second = ta.getFolder("b") / "second.laz";
    fs::copy_file(pc, first, fs::copy_options::overwrite_existing);
    fs::copy_file(pc, second, fs::copy_options::overwrite_existing);

    ddb::PointCloudInfo source;
    ASSERT_TRUE(ddb::getPointCloudInfo(pc.string(), source));
    expectPointsOf(source, buildAndRead({first.string(), second.string()}, ta.getFolder("copc")), 2);
}

// A cloud of one point, or of no extent in one or two axes (a vertical line, a vertical wall), has a
// COPC of all its points: PDAL writes and reads such LAS and PLY files, and the octree cube of the
// builder is the largest extent of the cloud plus one scale step on each side, so it never has a size
// of 0. The COPC reads back and renders a thumbnail. The count and the bounds are those of the points
// the builder read, whatever the format of the file. A cloud of no point has no COPC.
TEST(pointcloud, aCloudOfOnePointOrNoExtentInAnAxisHasACopcAndAnEmptyCloudHasNone) {
    TestArea ta(TEST_NAME);
    const auto expectThumbnailOf = [&ta](const fs::path& folder) {
        const fs::path thumb = ta.getPath(folder.filename().string() + ".webp");
        ddb::generateThumb(folder / "cloud.copc.laz", 256, thumb, true);
        EXPECT_GT(fs::exists(thumb) ? fs::file_size(thumb) : 0, 0u);
    };
    const std::vector<std::pair<std::string, std::string>> clouds = {
        {"one.xyz", "1 2 3\n"},
        {"vertical.xyz", "1 2 3\n1 2 5\n1 2 7\n"},
        {"line.pts", "2\n1 2 3\n1 5 3\n"},
        {"wall.xyz", "0 0 0\n0 1 0\n0 0 1\n0 1 1\n"}};
    for (const auto& [name, text] : clouds) {
        SCOPED_TRACE(name);
        const fs::path file = ta.getPath(name);
        std::ofstream(file) << text;
        const fs::path out = ta.getFolder("copc-" + fs::path(name).stem().string());
        expectPointsOf(readTextCloud(file), buildAndRead({file.string()}, out), 1e-3);
        expectThumbnailOf(out);
    }

    // The single return of a LAS file
    const fs::path one = ta.getPath("one.las");
    testutils::writeLas14(one, "", {{500000.5, 4100000.25, 120.0}});
    ddb::PointCloudInfo source;
    ASSERT_TRUE(ddb::getPointCloudInfo(one.string(), source));
    ASSERT_EQ(source.pointCount, 1);
    const fs::path oneOut = ta.getFolder("copc-one-las");
    expectPointsOf(source, buildAndRead({one.string()}, oneOut));
    expectThumbnailOf(oneOut);

    const fs::path empty = ta.getPath("empty.las");
    testutils::writeLas14(empty, "", {});
    const fs::path emptyOut = ta.getFolder("copc-empty");
    try {
        ddb::buildCopc({empty.string()}, emptyOut.string());
        ADD_FAILURE() << "a cloud of no point has no COPC";
    } catch (const ddb::AppException& e) {
        EXPECT_NE(std::string(e.what()).find("no points"), std::string::npos) << e.what();
    }
    expectNothingIn(emptyOut);
}

// A file the builder cannot read is a point cloud that cannot be read, as PDAL's build reported it,
// and leaves nothing
TEST(pointcloud, aCloudTheBuilderCannotReadIsAPdalException) {
    TestArea ta(TEST_NAME);
    const std::vector<std::pair<std::string, std::string>> clouds = {
        {"header_only.xyz", "X Y Z\n"}, {"text.las", "this is not a point cloud\n"}};
    for (const auto& [name, text] : clouds) {
        SCOPED_TRACE(name);
        const fs::path file = ta.getPath(name);
        std::ofstream(file) << text;
        ASSERT_EQ(ddb::fingerprint(file), ddb::EntryType::PointCloud);
        const fs::path out = ta.getFolder("copc-" + fs::path(name).stem().string());
        EXPECT_THROW(ddb::buildCopc({file.string()}, out.string()), ddb::PDALException);
        expectNothingIn(out);
    }
}

// A file that is not a point cloud, or that does not exist, is refused before the helper runs
TEST(pointcloud, aSourceThatIsNoPointCloudIsRefusedBeforeTheBuild) {
    TestArea ta(TEST_NAME);
    const fs::path notes = ta.getPath("notes.txt");
    std::ofstream(notes) << "1 2 3\n4 5 6\n";
    const fs::path out = ta.getFolder("copc");
    EXPECT_THROW(ddb::buildCopc({notes.string()}, out.string()), ddb::InvalidArgsException);
    EXPECT_THROW(ddb::buildCopc({ta.getPath("missing.laz").string()}, out.string()), ddb::FSException);
    EXPECT_THROW(ddb::buildCopc({}, out.string()), ddb::InvalidArgsException);
    expectNothingIn(out);
}

// The build reports the phases of the builder, from the plan at 0 to the end of the file at 1
TEST(pointcloud, theCopcBuildReportsThePhasesOfTheBuilder) {
    TestArea ta(TEST_NAME);
    const fs::path pc = ta.downloadTestAsset(testDataUrl("brighton/point_cloud.laz"), "point_cloud.laz");

    std::vector<std::pair<double, std::string>> reports;
    ddb::BuildOptions options;
    options.progress = [&reports](double fraction, std::string_view phase) {
        reports.emplace_back(fraction, std::string(phase));
    };
    ddb::buildCopc({pc.string()}, ta.getFolder("copc").string(), options);

    ASSERT_GE(reports.size(), 3u);
    EXPECT_EQ(reports.front(), std::make_pair(0.0, std::string("planning")));
    EXPECT_EQ(reports.back(), std::make_pair(1.0, std::string("writing")));
    const std::vector<std::string> order = {"planning", "reading", "building", "writing"};
    size_t last = 0;
    bool building = false;
    for (size_t i = 0; i < reports.size(); i++) {
        SCOPED_TRACE(i);
        if (i > 0) {
            EXPECT_GE(reports[i].first, reports[i - 1].first);
        }
        const size_t at = static_cast<size_t>(std::find(order.begin(), order.end(), reports[i].second) - order.begin());
        ASSERT_LT(at, order.size()) << reports[i].second;
        EXPECT_GE(at, last) << "the phases go in order";
        last = at;
        building = building || reports[i].second == "building";
    }
    EXPECT_TRUE(building);
}

// The phase of a fraction is the one ddb-tilers reports with it
TEST(pointcloud, thePhaseOfAFractionIsTheOneOfTheBuilder) {
    TestArea ta(TEST_NAME);
    const fs::path pc = ta.downloadTestAsset(testDataUrl("brighton/point_cloud.laz"), "point_cloud.laz");

    std::vector<std::pair<double, std::string>> reports;
    const ddbtProgressCallback record = [](double fraction, const char* phase, void* user) {
        static_cast<std::vector<std::pair<double, std::string>>*>(user)->emplace_back(fraction, phase);
    };
    const std::string out = (ta.getFolder("copc") / "cloud.copc.laz").string();
    ASSERT_EQ(ddbtBuildCopc2(pc.string().c_str(), out.c_str(), R"({"threads": 2, "metadata": false})", record, &reports),
              DDBT_ERR_NONE)
        << ddbtLastError();

    std::vector<std::string> phases;
    for (const auto& [fraction, phase] : reports) {
        SCOPED_TRACE(fraction);
        EXPECT_STREQ(ddb::copcPhaseOfFraction(fraction), phase.c_str());
        if (phases.empty() || phases.back() != phase)
            phases.push_back(phase);
    }
    EXPECT_EQ(phases, (std::vector<std::string>{"planning", "reading", "building", "writing"}));
}

}  // namespace
