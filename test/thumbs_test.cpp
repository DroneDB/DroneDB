/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <fstream>
#include "gtest/gtest.h"
#include "thumbs.h"
#include "hash.h"
#include "mio.h"
#include "pointcloud.h"
#include "test.h"
#include "testarea.h"

namespace {

using namespace ddb;

TEST(thumbnail, ortho) {
    TestArea ta(TEST_NAME);
    fs::path ortho = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/brighton/odm_orthophoto.tif",
                                          "odm_orthophoto.tif");

    fs::path outFile = ta.getPath("output.jpg");
    ddb::generateThumb(ortho.string(), 256, outFile, true);


    uint8_t *buffer;
    unsigned long long bufSize;
    ddb::generateThumb(ortho.string(), 256, "", true, &buffer, &bufSize);

    EXPECT_TRUE(bufSize > 0);
    EXPECT_EQ(io::Path(outFile).getSize(), bufSize);

    fs::path outMemoryFile = ta.getPath("output-memory.jpg");

    std::ofstream of(outMemoryFile.string(), std::ios::out | std::ios::binary | std::ios::trunc);
    of.write(reinterpret_cast<char *>(buffer), bufSize);
    of.close();

    EXPECT_EQ(Hash::fileSHA256(outMemoryFile.string()),
              Hash::fileSHA256(outFile.string()));

    VSIFree(buffer);
}

TEST(thumbnail, ept) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/brighton/point_cloud.laz",
                                          "point_cloud.laz");

    ddb::buildEpt({ pc.string() }, ta.getFolder("ept"));

    fs::path outFile = ta.getPath("output.jpg");
    fs::path eptPath = ta.getPath(fs::path("ept") / "ept.json");

    ddb::generateThumb(eptPath, 256, outFile, true);

    uint8_t *buffer;
    unsigned long long bufSize;
    ddb::generateThumb(eptPath, 256, "", true, &buffer, &bufSize);

    EXPECT_TRUE(bufSize > 0);
    EXPECT_EQ(io::Path(outFile).getSize(), bufSize);

    fs::path outMemoryFile = ta.getPath("output-memory.jpg");

    std::ofstream of(outMemoryFile.string(), std::ios::out | std::ios::binary | std::ios::trunc);
    of.write(reinterpret_cast<char *>(buffer), bufSize);
    of.close();

    EXPECT_EQ(Hash::fileSHA256(outMemoryFile.string()),
              Hash::fileSHA256(outFile.string()));

    VSIFree(buffer);
}




}
