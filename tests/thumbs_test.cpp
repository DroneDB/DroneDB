/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "thumbs.h"

#include <fstream>

#include "ddb.h"
#include "gtest/gtest.h"
#include "hash.h"
#include "mio.h"
#include "pointcloud.h"
#include "test.h"
#include "testarea.h"

namespace {

using namespace ddb;

TEST(thumbnail, ortho) {
    TestArea ta(TEST_NAME);
    fs::path ortho = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/master/brighton/odm_orthophoto.tif",
        "odm_orthophoto.tif");    fs::path outFile = ta.getPath("output.webp");
    ddb::generateThumb(ortho.string(), 256, outFile, true);

    uint8_t* buffer;
    int bufSize;
    ddb::generateThumb(ortho.string(), 256, "", true, &buffer, &bufSize);

    EXPECT_TRUE(bufSize > 0);
    EXPECT_EQ(io::Path(outFile).getSize(), bufSize);

    fs::path outMemoryFile = ta.getPath("output-memory.webp");

    std::ofstream of(outMemoryFile.string(), std::ios::out | std::ios::binary | std::ios::trunc);
    of.write(reinterpret_cast<char*>(buffer), bufSize);
    of.close();

    EXPECT_EQ(Hash::fileSHA256(outMemoryFile.string()), Hash::fileSHA256(outFile.string()));

    DDBVSIFree(buffer);
}

TEST(thumbnail, ept_file) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/master/brighton/point_cloud.laz",
        "point_cloud.laz");

    ddb::buildEpt({pc.string()}, ta.getFolder("ept").string());

    fs::path outFile = ta.getPath("output.webp");
    fs::path eptPath = ta.getPath(fs::path("ept") / "ept.json");

    ddb::generateThumb(eptPath, 256, outFile, true);

    EXPECT_TRUE(fs::exists(outFile));
    EXPECT_TRUE(io::Path(outFile).getSize() > 0);
}

TEST(thumbnail, ept_memory) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/master/brighton/point_cloud.laz",
        "point_cloud.laz");

    ddb::buildEpt({pc.string()}, ta.getFolder("ept").string());

    fs::path eptPath = ta.getPath(fs::path("ept") / "ept.json");

    uint8_t* buffer;
    int bufSize;
    ddb::generateThumb(eptPath, 256, "", true, &buffer, &bufSize);

    EXPECT_TRUE(bufSize > 0);
    EXPECT_TRUE(buffer != nullptr);

    // Test writing to file and comparing
    fs::path outMemoryFile = ta.getPath("output-memory.webp");
    std::ofstream of(outMemoryFile.string(), std::ios::out | std::ios::binary | std::ios::trunc);
    of.write(reinterpret_cast<char*>(buffer), bufSize);
    of.close();

    EXPECT_EQ(io::Path(outMemoryFile).getSize(), bufSize);

    DDBVSIFree(buffer);
}

TEST(thumbnail, lewis_file) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/refs/heads/master/point-clouds/lewis.laz",
        "lewis.laz");

    ddb::buildEpt({pc.string()}, ta.getFolder("ept").string());

    fs::path outFile = ta.getPath("output.webp");
    fs::path eptPath = ta.getPath(fs::path("ept") / "ept.json");

    ddb::generateThumb(eptPath, 256, outFile, true);

    EXPECT_TRUE(fs::exists(outFile));
    EXPECT_TRUE(io::Path(outFile).getSize() > 0);
}

TEST(thumbnail, toledo_file) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/refs/heads/master/point-clouds/toledo.laz",
        "toledo.laz");

    ddb::buildEpt({pc.string()}, ta.getFolder("ept").string());

    fs::path outFile = ta.getPath("output.webp");
    fs::path eptPath = ta.getPath(fs::path("ept") / "ept.json");

    ddb::generateThumb(eptPath, 256, outFile, true);

    EXPECT_TRUE(fs::exists(outFile));
    EXPECT_TRUE(io::Path(outFile).getSize() > 0);
}

}  // namespace
