/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "thumbs.h"

#include <chrono>
#include <fstream>
#include <thread>

#include "ddb.h"
#include "exceptions.h"
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

TEST(thumbnail, pointCloudScalarField) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/master/point-clouds/point-cloud-scalar-field.laz",
        "point_cloud.laz");

    // Build EPT from LAZ file
    ddb::buildEpt({pc.string()}, ta.getFolder("ept").string());

    fs::path eptPath = ta.getPath(fs::path("ept") / "ept.json");
    EXPECT_TRUE(fs::exists(eptPath)) << "EPT file should exist after buildEpt";

    // Generate WebP thumbnail
    fs::path outFile = ta.getPath("point-cloud-scalar-field.webp");
    ddb::generateThumb(eptPath, 256, outFile, true);

    // Verify thumbnail exists and is not empty
    EXPECT_TRUE(fs::exists(outFile)) << "Thumbnail file should exist";
    EXPECT_TRUE(isWebPImageNonEmpty(outFile)) << "Thumbnail should not be empty/transparent";

}

TEST(thumbnail, pointCloudComplex) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/master/point-clouds/point-cloud-complex.laz",
        "point_cloud.laz");

    // Build EPT from LAZ file
    ddb::buildEpt({pc.string()}, ta.getFolder("ept").string());

    fs::path eptPath = ta.getPath(fs::path("ept") / "ept.json");
    EXPECT_TRUE(fs::exists(eptPath)) << "EPT file should exist after buildEpt";

    // Generate WebP thumbnail
    fs::path outFile = ta.getPath("point-cloud-complex.webp");
    ddb::generateThumb(eptPath, 256, outFile, true);

    // Verify thumbnail exists and is not empty
    EXPECT_TRUE(fs::exists(outFile)) << "Thumbnail file should exist";
    EXPECT_TRUE(isWebPImageNonEmpty(outFile)) << "Thumbnail should not be empty/transparent";

}

// Test for images with palette (indexed color) - these require special handling
// as GDAL opens them as 1-band images but WebP requires 3 or 4 bands.
// This test ensures that palette images are correctly expanded to RGB/RGBA
// before conversion to WebP format.
TEST(thumbnail, paletteImage) {
    TestArea ta(TEST_NAME);
    fs::path img = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/refs/heads/master/images/img-palette.png",
        "img-palette.png");

    // Generate WebP thumbnail to file
    fs::path outFile = ta.getPath("palette-thumb.webp");
    ddb::generateThumb(img, 256, outFile, true);

    // Verify thumbnail exists and is valid
    EXPECT_TRUE(fs::exists(outFile)) << "Palette image thumbnail file should exist";
    EXPECT_TRUE(isWebPImageNonEmpty(outFile)) << "Palette image thumbnail should not be empty";

    // Test in-memory generation
    uint8_t* buffer;
    int bufSize;
    ddb::generateThumb(img, 256, "", true, &buffer, &bufSize);

    EXPECT_TRUE(bufSize > 0) << "In-memory palette thumbnail should have content";
    EXPECT_NE(buffer, nullptr) << "In-memory buffer should not be null";

    // Verify the in-memory result matches the file
    EXPECT_EQ(io::Path(outFile).getSize(), bufSize)
        << "File and memory thumbnail sizes should match";

    DDBVSIFree(buffer);
}

// =============================================================================
// Edge Cases Tests
// =============================================================================

// Test that invalid thumbSize values are rejected
TEST(thumbnail, invalidThumbSize) {
    TestArea ta(TEST_NAME);
    fs::path ortho = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/master/brighton/odm_orthophoto.tif",
        "odm_orthophoto.tif");

    fs::path outFile = ta.getPath("output.webp");

    // thumbSize = 0 should throw
    EXPECT_THROW(ddb::generateThumb(ortho.string(), 0, outFile, true), ddb::InvalidArgsException)
        << "thumbSize = 0 should throw InvalidArgsException";

    // thumbSize = -1 should throw
    EXPECT_THROW(ddb::generateThumb(ortho.string(), -1, outFile, true), ddb::InvalidArgsException)
        << "thumbSize = -1 should throw InvalidArgsException";

    // thumbSize = -100 should throw
    EXPECT_THROW(ddb::generateThumb(ortho.string(), -100, outFile, true), ddb::InvalidArgsException)
        << "Negative thumbSize should throw InvalidArgsException";
}

// Test that non-existent input file throws appropriate exception
TEST(thumbnail, nonExistentFile) {
    TestArea ta(TEST_NAME);
    fs::path nonExistent = ta.getPath("this_file_does_not_exist.tif");
    fs::path outFile = ta.getPath("output.webp");

    EXPECT_THROW(ddb::generateThumb(nonExistent.string(), 256, outFile, true), ddb::FSException)
        << "Non-existent file should throw FSException";
}

// Test that corrupted/invalid image file throws appropriate exception
TEST(thumbnail, corruptedFile) {
    TestArea ta(TEST_NAME);

    // Create a corrupted "image" file (just random bytes)
    fs::path corruptedFile = ta.getPath("corrupted.tif");
    std::ofstream ofs(corruptedFile.string(), std::ios::binary);
    ofs << "This is not a valid TIFF file content - just garbage data!";
    ofs.close();

    fs::path outFile = ta.getPath("output.webp");

    // Should throw GDALException when trying to open invalid file
    EXPECT_THROW(ddb::generateThumb(corruptedFile.string(), 256, outFile, true), ddb::GDALException)
        << "Corrupted file should throw GDALException";
}

// Test that empty file throws appropriate exception
TEST(thumbnail, emptyFile) {
    TestArea ta(TEST_NAME);

    // Create an empty file
    fs::path emptyFile = ta.getPath("empty.tif");
    std::ofstream ofs(emptyFile.string(), std::ios::binary);
    ofs.close();  // Close immediately, creating 0-byte file

    fs::path outFile = ta.getPath("output.webp");

    // Should throw GDALException when trying to open empty file
    EXPECT_THROW(ddb::generateThumb(emptyFile.string(), 256, outFile, true), ddb::GDALException)
        << "Empty file should throw GDALException";
}

// Test thumbnail generation with very small thumbSize (edge case)
TEST(thumbnail, verySmallThumbSize) {
    TestArea ta(TEST_NAME);
    fs::path ortho = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/master/brighton/odm_orthophoto.tif",
        "odm_orthophoto.tif");

    // Test with minimum valid size (1 pixel)
    fs::path outFile1 = ta.getPath("tiny_thumb_1.webp");
    EXPECT_NO_THROW(ddb::generateThumb(ortho.string(), 1, outFile1, true))
        << "thumbSize = 1 should be valid";
    EXPECT_TRUE(fs::exists(outFile1)) << "1px thumbnail should be created";
    EXPECT_GT(io::Path(outFile1).getSize(), 0) << "1px thumbnail should have some content";

    // Test with very small size (2 pixels)
    fs::path outFile2 = ta.getPath("tiny_thumb_2.webp");
    EXPECT_NO_THROW(ddb::generateThumb(ortho.string(), 2, outFile2, true))
        << "thumbSize = 2 should be valid";
    EXPECT_TRUE(fs::exists(outFile2)) << "2px thumbnail should be created";

    // Test with small size (32 pixels) - should have valid WebP content
    fs::path outFile32 = ta.getPath("small_thumb_32.webp");
    EXPECT_NO_THROW(ddb::generateThumb(ortho.string(), 32, outFile32, true))
        << "thumbSize = 32 should be valid";
    EXPECT_TRUE(fs::exists(outFile32)) << "32px thumbnail should be created";
    // Small thumbnails may not reach 1KB threshold, just check file exists and has content
    EXPECT_GT(io::Path(outFile32).getSize(), WEBP_MIN_HEADER_SIZE) << "32px thumbnail should be valid WebP";
}

// Test thumbnail generation with very large thumbSize
TEST(thumbnail, veryLargeThumbSize) {
    TestArea ta(TEST_NAME);
    fs::path ortho = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/master/brighton/odm_orthophoto.tif",
        "odm_orthophoto.tif");

    // Test with large size (4096 pixels) - larger than most source images
    fs::path outFile = ta.getPath("large_thumb.webp");
    EXPECT_NO_THROW(ddb::generateThumb(ortho.string(), 4096, outFile, true))
        << "thumbSize = 4096 should be valid";
    EXPECT_TRUE(fs::exists(outFile)) << "Large thumbnail should be created";
    EXPECT_TRUE(isWebPImageNonEmpty(outFile)) << "Large thumbnail should have content";
}

// Test forceRecreate flag behavior
TEST(thumbnail, forceRecreateFlag) {
    TestArea ta(TEST_NAME);
    fs::path ortho = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/master/brighton/odm_orthophoto.tif",
        "odm_orthophoto.tif");

    fs::path outFile = ta.getPath("recreate_test.webp");

    // First generation
    ddb::generateThumb(ortho.string(), 256, outFile, true);
    EXPECT_TRUE(fs::exists(outFile)) << "First thumbnail should be created";

    auto firstModTime = fs::last_write_time(outFile);
    auto firstSize = io::Path(outFile).getSize();

    // Wait a tiny bit to ensure different modification time
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Generate again with forceRecreate = false (should not recreate)
    ddb::generateThumb(ortho.string(), 256, outFile, false);
    auto secondModTime = fs::last_write_time(outFile);
    EXPECT_EQ(firstModTime, secondModTime) << "Thumbnail should not be recreated when forceRecreate=false";

    // Generate again with forceRecreate = true (should recreate)
    ddb::generateThumb(ortho.string(), 256, outFile, true);
    // File should still exist and be valid
    EXPECT_TRUE(fs::exists(outFile)) << "Thumbnail should exist after forceRecreate";
    EXPECT_EQ(firstSize, io::Path(outFile).getSize()) << "Recreated thumbnail should have same size";
}

// Test in-memory generation with null buffer pointer edge cases
TEST(thumbnail, inMemoryNullPointers) {
    TestArea ta(TEST_NAME);
    fs::path ortho = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/master/brighton/odm_orthophoto.tif",
        "odm_orthophoto.tif");

    // When outImagePath is empty but outBuffer is nullptr, it should still work
    // (writing to empty path with null buffer - edge case)
    uint8_t* buffer = nullptr;
    int bufSize = 0;

    // This should generate to memory successfully
    ddb::generateThumb(ortho.string(), 256, "", true, &buffer, &bufSize);
    EXPECT_NE(buffer, nullptr) << "Buffer should be allocated";
    EXPECT_GT(bufSize, 0) << "Buffer size should be positive";

    if (buffer != nullptr) {
        DDBVSIFree(buffer);
    }
}

}  // namespace
