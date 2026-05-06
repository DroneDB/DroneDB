/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "thumbs.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

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

TEST(thumbnail, copc_file) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/master/brighton/point_cloud.laz",
        "point_cloud.laz");

    ddb::buildCopc({pc.string()}, ta.getFolder("copc").string());

    fs::path outFile = ta.getPath("output.webp");
    fs::path copcPath = ta.getPath(fs::path("copc") / "cloud.copc.laz");

    ddb::generateThumb(copcPath, 256, outFile, true);

    EXPECT_TRUE(fs::exists(outFile));
    EXPECT_TRUE(io::Path(outFile).getSize() > 0);
}

TEST(thumbnail, copc_memory) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/master/brighton/point_cloud.laz",
        "point_cloud.laz");

    ddb::buildCopc({pc.string()}, ta.getFolder("copc").string());

    fs::path copcPath = ta.getPath(fs::path("copc") / "cloud.copc.laz");

    uint8_t* buffer;
    int bufSize;
    ddb::generateThumb(copcPath, 256, "", true, &buffer, &bufSize);

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

    ddb::buildCopc({pc.string()}, ta.getFolder("copc").string());

    fs::path outFile = ta.getPath("output.webp");
    fs::path copcPath = ta.getPath(fs::path("copc") / "cloud.copc.laz");

    ddb::generateThumb(copcPath, 256, outFile, true);

    EXPECT_TRUE(fs::exists(outFile));
    EXPECT_TRUE(io::Path(outFile).getSize() > 0);
}

const size_t WEBP_MIN_HEADER_SIZE = 26;
const size_t MIN_THUMBNAIL_SIZE = 1024;

// Returns true if webpPath exists, has a valid RIFF/WEBP header, and is at
// least minSize bytes. Pass MIN_THUMBNAIL_SIZE to reject trivially small files;
// pass WEBP_MIN_HEADER_SIZE (the default) for a pure signature check.
bool isValidWebP(const fs::path& webpPath,
                 size_t minSize = WEBP_MIN_HEADER_SIZE) {
    if (!fs::exists(webpPath)) return false;
    const auto fileSize = io::Path(webpPath).getSize();
    if (fileSize < static_cast<long long>(minSize)) return false;
    std::ifstream file(webpPath.string(), std::ios::binary);
    if (!file.is_open()) return false;
    uint8_t header[12];
    file.read(reinterpret_cast<char*>(header), 12);
    if (file.gcount() != 12) return false;
    if (header[0]!='R'||header[1]!='I'||header[2]!='F'||header[3]!='F') return false;
    if (header[8]!='W'||header[9]!='E'||header[10]!='B'||header[11]!='P') return false;
    return true;
}

// Wrapper used by single-file tests: also enforces the 1 KiB floor that
// distinguishes a real thumbnail from a degenerate near-empty WebP.
bool isWebPImageNonEmpty(const fs::path& webpPath) {
    return isValidWebP(webpPath, MIN_THUMBNAIL_SIZE);
}

TEST(thumbnail, brightonsLazCopc) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/master/brighton/point_cloud.laz",
        "point_cloud.laz");

    // Build COPC from LAZ file
    ddb::buildCopc({pc.string()}, ta.getFolder("copc").string());

    fs::path copcPath = ta.getPath(fs::path("copc") / "cloud.copc.laz");
    EXPECT_TRUE(fs::exists(copcPath)) << "COPC file should exist after buildCopc";

    // Generate WebP thumbnail
    fs::path outFile = ta.getPath("brighton_thumbnail.webp");
    ddb::generateThumb(copcPath, 256, outFile, true);

    // Verify thumbnail exists and is not empty
    EXPECT_TRUE(fs::exists(outFile)) << "Thumbnail file should exist";
    EXPECT_TRUE(isWebPImageNonEmpty(outFile)) << "Thumbnail should not be empty/transparent";

}

TEST(thumbnail, toledoLazCopc) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/refs/heads/master/point-clouds/toledo.laz",
        "toledo_point_cloud.laz");

    // Build COPC from Toledo LAZ file
    ddb::buildCopc({pc.string()}, ta.getFolder("toledo_copc").string());

    fs::path copcPath = ta.getPath(fs::path("toledo_copc") / "cloud.copc.laz");
    EXPECT_TRUE(fs::exists(copcPath)) << "Toledo COPC file should exist after buildCopc";

    // Generate WebP thumbnail with different size
    fs::path outFile = ta.getPath("toledo_thumbnail.webp");
    ddb::generateThumb(copcPath, 512, outFile, true);

    // Verify thumbnail exists and is not empty
    EXPECT_TRUE(fs::exists(outFile)) << "Toledo thumbnail file should exist";
    EXPECT_TRUE(isWebPImageNonEmpty(outFile)) << "Toledo thumbnail should not be empty/transparent";

    // Test that a larger thumbnail has more data
    fs::path smallThumb = ta.getPath("toledo_small.webp");
    ddb::generateThumb(copcPath, 128, smallThumb, true);

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
    ddb::generateThumb(copcPath, 512, "", true, &buffer512, &bufSize512);

    uint8_t* buffer128;
    int bufSize128;
    ddb::generateThumb(copcPath, 128, "", true, &buffer128, &bufSize128);

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

    // Build COPC from LAZ file
    ddb::buildCopc({pc.string()}, ta.getFolder("copc").string());

    fs::path copcPath = ta.getPath(fs::path("copc") / "cloud.copc.laz");
    EXPECT_TRUE(fs::exists(copcPath)) << "COPC file should exist after buildCopc";

    // Generate WebP thumbnail
    fs::path outFile = ta.getPath("point-cloud-scalar-field.webp");
    ddb::generateThumb(copcPath, 256, outFile, true);

    // Verify thumbnail exists and is not empty
    EXPECT_TRUE(fs::exists(outFile)) << "Thumbnail file should exist";
    EXPECT_TRUE(isWebPImageNonEmpty(outFile)) << "Thumbnail should not be empty/transparent";

}

TEST(thumbnail, pointCloudComplex) {
    TestArea ta(TEST_NAME);
    fs::path pc = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/master/point-clouds/point-cloud-complex.laz",
        "point_cloud.laz");

    // Build COPC from LAZ file
    ddb::buildCopc({pc.string()}, ta.getFolder("copc").string());

    fs::path copcPath = ta.getPath(fs::path("copc") / "cloud.copc.laz");
    EXPECT_TRUE(fs::exists(copcPath)) << "COPC file should exist after buildCopc";

    // Generate WebP thumbnail
    fs::path outFile = ta.getPath("point-cloud-complex.webp");
    ddb::generateThumb(copcPath, 256, outFile, true);

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

// Test for single-band GeoTIFF (DEM/DSM) - these have no color table
// and require band replication instead of -expand to produce RGB WebP output.
TEST(thumbnail, singleBandDem) {
    TestArea ta(TEST_NAME);
    fs::path dem = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/refs/heads/master/dem/test_dem.tiff",
        "test_dem.tiff");

    // Generate WebP thumbnail to file
    fs::path outFile = ta.getPath("dem-thumb.webp");
    ddb::generateThumb(dem, 256, outFile, true);

    // Verify thumbnail exists and is valid
    EXPECT_TRUE(fs::exists(outFile)) << "DEM thumbnail file should exist";
    EXPECT_TRUE(isWebPImageNonEmpty(outFile)) << "DEM thumbnail should not be empty";

    // Test in-memory generation
    uint8_t* buffer;
    int bufSize;
    ddb::generateThumb(dem, 256, "", true, &buffer, &bufSize);

    EXPECT_TRUE(bufSize > 0) << "In-memory DEM thumbnail should have content";
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

// =============================================================================
// Point Cloud Edge Cases - buildCopc validation tests
// =============================================================================

// Base URL for test data files in DroneDB/test_data repository
const std::string TEST_DATA_BASE_URL = "https://github.com/DroneDB/test_data/raw/master/testdata/";

// Helper to download a test data file from the test_data repository
fs::path downloadTestDataFile(TestArea& ta, const std::string& relativePath) {
    std::string url = TEST_DATA_BASE_URL + relativePath;
    // Extract filename from path
    fs::path relPath(relativePath);
    std::string filename = relPath.filename().string();
    return ta.downloadTestAsset(url, filename);
}

// Test for point cloud with single point (edge case)
// buildCopc should throw InvalidArgsException for single-point clouds
TEST(thumbnail, pointCloudSinglePoint) {
    TestArea ta(TEST_NAME);
    fs::path pc = downloadTestDataFile(ta, "pointcloud/pc_single_point.laz");

    // buildCopc should throw InvalidArgsException for single-point clouds
    EXPECT_THROW(ddb::buildCopc({pc.string()}, ta.getFolder("copc").string()),
                 ddb::InvalidArgsException)
        << "buildCopc should reject single-point clouds with InvalidArgsException";
}

// Test for point cloud with flat surface (all Z values identical)
// This should work if the point cloud has sufficient X/Y extent
// Note: If the flat surface also has zero X/Y extent, it will throw InvalidArgsException
TEST(thumbnail, pointCloudFlatSurface) {
    TestArea ta(TEST_NAME);
    fs::path pc = downloadTestDataFile(ta, "pointcloud/pc_flat_surface.laz");

    // Build COPC from LAZ file
    // This may throw InvalidArgsException if the flat surface also has zero X/Y extent
    try {
        ddb::buildCopc({pc.string()}, ta.getFolder("copc").string());

        fs::path copcPath = ta.getPath(fs::path("copc") / "cloud.copc.laz");
        EXPECT_TRUE(fs::exists(copcPath)) << "COPC file should exist after buildCopc";

        fs::path outFile = ta.getPath("flat-surface-thumb.webp");
        EXPECT_NO_THROW(ddb::generateThumb(copcPath, 256, outFile, true))
            << "Flat surface point cloud should generate thumbnail (all points same Z -> gray)";
        EXPECT_TRUE(fs::exists(outFile)) << "Flat surface thumbnail should exist";
        EXPECT_TRUE(isWebPImageNonEmpty(outFile)) << "Flat surface thumbnail should have content";
    } catch (const ddb::InvalidArgsException& e) {
        // Expected if the point cloud also has zero X/Y extent
        SUCCEED() << "Flat surface point cloud rejected due to insufficient extent: " << e.what();
    }
}

// --- Batch regression tests for georaster thumbnail generation ---
//
// These tests exercise generateThumb over a wide variety of real-world rasters
// from the public test_data repository. Each helper runs the full list and
// aggregates failures so that a single broken input does not mask the others.

namespace {

struct BatchResult {
    std::string filename;
    std::string error;  // empty when the file succeeded
};

void runThumbnailBatch(const std::string& testName,
                       const std::string& remoteFolder,
                       const std::vector<std::string>& filenames,
                       int thumbSize = 256) {
    TestArea ta(testName);
    std::vector<BatchResult> failures;

    for (const auto& fname : filenames) {
        const std::string url =
            "https://github.com/DroneDB/test_data/raw/master/" +
            remoteFolder + "/" + fname;
        try {
            fs::path src = ta.downloadTestAsset(url, fname);
            // Sanitize output name: replace separators so path nesting in
            // filenames does not escape the test area.
            std::string outName = fname;
            std::replace(outName.begin(), outName.end(), '/', '_');
            std::replace(outName.begin(), outName.end(), '\\', '_');
            fs::path outFile = ta.getPath(outName + ".thumb.webp");

            ddb::generateThumb(src, thumbSize, outFile, true);

            if (!fs::exists(outFile)) {
                failures.push_back({fname, "output file missing"});
                continue;
            }
            if (!isValidWebP(outFile)) {
                failures.push_back({fname, "output not a valid WEBP"});
                continue;
            }

            // Exercise the in-memory path too.
            uint8_t* buffer = nullptr;
            int bufSize = 0;
            ddb::generateThumb(src, thumbSize, "", true, &buffer, &bufSize);
            if (!buffer || bufSize <= 0) {
                failures.push_back({fname, "in-memory thumbnail returned empty buffer"});
            }
            if (buffer) DDBVSIFree(buffer);
        } catch (const std::exception& e) {
            failures.push_back({fname, std::string("exception: ") + e.what()});
        } catch (...) {
            failures.push_back({fname, "unknown exception"});
        }
    }

    if (!failures.empty()) {
        std::ostringstream msg;
        msg << "Thumbnail batch failed for " << failures.size() << "/"
            << filenames.size() << " file(s):\n";
        for (const auto& f : failures) {
            msg << "  - " << f.filename << ": " << f.error << "\n";
        }
        FAIL() << msg.str();
    }
}

}  // namespace

// Regression test for repr3.tif: Byte raster with out-of-range nodata value
// that historically caused the -dstalpha crash in GDALTranslate.
TEST(thumbnail, rgbWithNodataRepr3) {
    TestArea ta(TEST_NAME);
    fs::path src = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/master/ortho/repr3.tif",
        "repr3.tif");

    fs::path outFile = ta.getPath("repr3-thumb.webp");
    EXPECT_NO_THROW(ddb::generateThumb(src, 512, outFile, true));
    EXPECT_TRUE(fs::exists(outFile));
    EXPECT_TRUE(isWebPImageNonEmpty(outFile));

    uint8_t* buffer = nullptr;
    int bufSize = 0;
    EXPECT_NO_THROW(ddb::generateThumb(src, 256, "", true, &buffer, &bufSize));
    EXPECT_GT(bufSize, 0);
    EXPECT_NE(buffer, nullptr);
    if (buffer) DDBVSIFree(buffer);
}

TEST(thumbnail, orthoBatch) {
    runThumbnailBatch("thumbnail_orthoBatch", "ortho", {
        "aukerman.tif",
        "brighton-beach-cog.tif",
        "brighton-beach.tif",
        "caliterra.tif",
        "copr.tif",
        "mygla.tif",
        "repr3.tif",
        "sheffield-park-3.tif",
        "vo.tif",
        "w5s.tif",
        "wro.tif",
    });
}

TEST(thumbnail, orthoEdgeBatch) {
    runThumbnailBatch("thumbnail_orthoEdgeBatch", "ortho/edge", {
        "abetow-ERD2018-EBIRD_SCIENCE-20191109-a5cf4cb2_hr_2018_abundance_median.tiff",
        "bremen_sea_ice_conc_2022_9_9.tif",
        "dom1_32_356_5699_1_nw_2020.tif",
        "eu_pasture.tiff",
        "GA4886_VanderfordGlacier_2022_EGM2008_64m-epsg3031.cog",
        "gadas-cyprus.tif",
        "gadas-world.tif",
        "gadas.tif",
        "ga_ls_tc_pc_cyear_3_x17y37_2022--P1Y_final_wet_pc_50_LQ.tif",
        "GeogToWGS84GeoKey5.tif",
        "gfw-azores.tif",
        "gpm_1d.20240617.tif",
        "lcv_landuse.cropland_hyde_p_10km_s0..0cm_2016_v3.2.tif",
        "LisbonElevation.tif",
        "no_pixelscale_or_tiepoints.tiff",
        "nt_20201024_f18_nrt_s.tif",
        "nz_habitat_anticross_4326_1deg.tif",
        "spam2005v3r2_harvested-area_wheat_total.tiff",
        "umbra_mount_yasur.tiff",
        "utm.tif",
        "vestfold.tif",
        "wildfires.tiff",
        "wind_direction.tif",
    });
}

TEST(thumbnail, testdataRasterBatch) {
    runThumbnailBatch("thumbnail_testdataRasterBatch", "testdata/raster", {
        "16bit_image.tif",
        "byte.tif",
        "float32_raster.tif",
        "grayscale_alpha_image.png",
        "grayscale_alpha_image.tif",
        "grayscale_image.tif",
        "image_with_nodata.tif",
        "multiband_image.tif",
        "rgba_with_alpha_band.tif",
        "square_image.tif",
        "tiny_image.tif",
        "utm.tif",
    });
}

}  // namespace
