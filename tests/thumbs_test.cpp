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
        "odm_orthophoto.tif");

    fs::path outFile = ta.getPath("output.jpg");
    ddb::generateThumb(ortho.string(), 256, outFile, true);

    uint8_t* buffer;
    int bufSize;
    ddb::generateThumb(ortho.string(), 256, "", true, &buffer, &bufSize);

    EXPECT_TRUE(bufSize > 0);
    EXPECT_EQ(io::Path(outFile).getSize(), bufSize);

    fs::path outMemoryFile = ta.getPath("output-memory.jpg");

    std::ofstream of(outMemoryFile.string(), std::ios::out | std::ios::binary | std::ios::trunc);
    of.write(reinterpret_cast<char*>(buffer), bufSize);
    of.close();

    EXPECT_EQ(Hash::fileSHA256(outMemoryFile.string()), Hash::fileSHA256(outFile.string()));

    DDBVSIFree(buffer);
}

TEST(thumbnail, ept) {

    // Disable this test if we are on windows (inconclusive)
    #ifdef _WIN32
    GTEST_SKIP() << "Skipping test on Windows";
    #endif    

    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/master/brighton/point_cloud.laz",
        "point_cloud.laz");

    ddb::buildEpt({pc.string()}, ta.getFolder("ept").string());

    fs::path outFile = ta.getPath("output.jpg");
    fs::path eptPath = ta.getPath(fs::path("ept") / "ept.json");

    ddb::generateThumb(eptPath, 256, outFile, true);

    uint8_t* buffer;
    int bufSize;
    ddb::generateThumb(eptPath, 256, "", true, &buffer, &bufSize);

    EXPECT_TRUE(bufSize > 0);
    EXPECT_EQ(io::Path(outFile).getSize(), bufSize);

    fs::path outMemoryFile = ta.getPath("output-memory.jpg");

    std::ofstream of(outMemoryFile.string(), std::ios::out | std::ios::binary | std::ios::trunc);
    of.write(reinterpret_cast<char*>(buffer), bufSize);
    of.close();

    const auto memoryFileHash = Hash::fileSHA256(outMemoryFile.string());
    const auto fileHash = Hash::fileSHA256(outFile.string());

    if (memoryFileHash != fileHash) {
        // Print exadecimal content of memory file and file
        std::ifstream inMemoryFile(outMemoryFile.string(), std::ios::binary);
        std::ifstream inFile(outFile.string(), std::ios::binary);
        std::ostringstream ossMemory;
        std::ostringstream ossFile;
        ossMemory << inMemoryFile.rdbuf();
        ossFile << inFile.rdbuf();
        std::string hexMemory = ossMemory.str();
        std::string hexFile = ossFile.str();
        std::cout << "Memory file hex: " << hexMemory << std::endl;
        std::cout << "File hex: " << hexFile << std::endl;
        std::cout << "Memory file size: " << io::Path(outMemoryFile).getSize() << std::endl;
        std::cout << "File size: " << io::Path(outFile).getSize() << std::endl;

    }

    EXPECT_EQ(memoryFileHash, fileHash);

    DDBVSIFree(buffer);
}

}  // namespace
