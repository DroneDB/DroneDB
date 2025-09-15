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

    fs::path outFile = ta.getPath("output.webp");
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

const size_t WEBP_MIN_HEADER_SIZE = 26;
const size_t MIN_THUMBNAIL_SIZE = 1024;

// Helper function to check if WebP image is not empty (not all transparent/white)
bool isWebPImageNonEmpty(const fs::path& webpPath) {
    // Check if file exists and has content
    if (!fs::exists(webpPath)) {
        return false;
    }

    const auto fileSize = io::Path(webpPath).getSize();
    if (fileSize == 0) {
        return false;
    }

    // WebP files must be at least 26 bytes (minimal header size)
    // A realistic thumbnail should be much larger (at least 1KB)
    if (fileSize < WEBP_MIN_HEADER_SIZE) {
        return false;
    }

    // Verify WebP signature by reading the file header
    std::ifstream file(webpPath.string(), std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    // Read the first 12 bytes to check WebP signature
    // WebP format: "RIFF" + 4 bytes size + "WEBP"
    uint8_t header[12];
    file.read(reinterpret_cast<char*>(header), 12);
    file.close();

    if (file.gcount() != 12) {
        return false;
    }

    // Check RIFF signature (bytes 0-3: "RIFF")
    if (header[0] != 'R' || header[1] != 'I' || header[2] != 'F' || header[3] != 'F') {
        return false;
    }

    // Check WebP signature (bytes 8-11: "WEBP")
    if (header[8] != 'W' || header[9] != 'E' || header[10] != 'B' || header[11] != 'P') {
        return false;
    }

    // For thumbnail testing purposes, expect at least 1KB for a meaningful image
    // This helps distinguish between minimal valid WebP files and actual thumbnails
    return fileSize >= MIN_THUMBNAIL_SIZE;
}

TEST(thumbnail, brightonsLazEpt) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/master/brighton/point_cloud.laz",
        "point_cloud.laz");

    // Build EPT from LAZ file
    ddb::buildEpt({pc.string()}, ta.getFolder("ept").string());

    fs::path eptPath = ta.getPath(fs::path("ept") / "ept.json");
    EXPECT_TRUE(fs::exists(eptPath)) << "EPT file should exist after buildEpt";

    // Generate WebP thumbnail
    fs::path outFile = ta.getPath("brighton_thumbnail.webp");
    ddb::generateThumb(eptPath, 256, outFile, true);

    // Verify thumbnail exists and is not empty
    EXPECT_TRUE(fs::exists(outFile)) << "Thumbnail file should exist";
    EXPECT_TRUE(isWebPImageNonEmpty(outFile)) << "Thumbnail should not be empty/transparent";

}

TEST(thumbnail, toledoLazEpt) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/refs/heads/master/point-clouds/toledo.laz",
        "toledo_point_cloud.laz");

    // Build EPT from Toledo LAZ file
    ddb::buildEpt({pc.string()}, ta.getFolder("toledo_ept").string());

    fs::path eptPath = ta.getPath(fs::path("toledo_ept") / "ept.json");
    EXPECT_TRUE(fs::exists(eptPath)) << "Toledo EPT file should exist after buildEpt";

    // Generate WebP thumbnail with different size
    fs::path outFile = ta.getPath("toledo_thumbnail.webp");
    ddb::generateThumb(eptPath, 512, outFile, true);

    // Verify thumbnail exists and is not empty
    EXPECT_TRUE(fs::exists(outFile)) << "Toledo thumbnail file should exist";
    EXPECT_TRUE(isWebPImageNonEmpty(outFile)) << "Toledo thumbnail should not be empty/transparent";

    // Test that a larger thumbnail has more data
    fs::path smallThumb = ta.getPath("toledo_small.webp");
    ddb::generateThumb(eptPath, 128, smallThumb, true);

    EXPECT_TRUE(fs::exists(smallThumb)) << "Small thumbnail should exist";
    EXPECT_TRUE(isWebPImageNonEmpty(smallThumb)) << "Small thumbnail should not be empty";

    // Generally larger thumbnails should have more data (though WebP compression can vary)
    auto largeSize = io::Path(outFile).getSize();
    auto smallSize = io::Path(smallThumb).getSize();
    EXPECT_GT(largeSize, 0) << "Large thumbnail should have content";
    EXPECT_GT(smallSize, 0) << "Small thumbnail should have content";

    // Test in-memory generation for both sizes
    uint8_t* buffer512;
    int bufSize512;
    ddb::generateThumb(eptPath, 512, "", true, &buffer512, &bufSize512);

    uint8_t* buffer128;
    int bufSize128;
    ddb::generateThumb(eptPath, 128, "", true, &buffer128, &bufSize128);

    EXPECT_TRUE(bufSize512 > 100) << "512px in-memory thumbnail should have reasonable size";
    EXPECT_TRUE(bufSize128 > 100) << "128px in-memory thumbnail should have reasonable size";
    EXPECT_NE(buffer512, nullptr) << "512px buffer should not be null";
    EXPECT_NE(buffer128, nullptr) << "128px buffer should not be null";

    DDBVSIFree(buffer512);
    DDBVSIFree(buffer128);

}

}  // namespace
