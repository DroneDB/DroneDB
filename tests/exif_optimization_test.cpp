/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <chrono>
#include <numeric>
#include "gtest/gtest.h"
#include "entry.h"
#include "test.h"
#include "testarea.h"
#include "logger.h"

namespace
{
    using namespace ddb;

    /**
     * @brief Test to measure performance of EXIF parsing.
     *
     * This test verifies the current behavior where Exiv2 is opened twice:
     * 1. In fingerprint() to determine the entry type (GeoImage vs Image)
     * 2. In parseEntry() to extract EXIF metadata
     *
     * The goal is to establish a baseline for optimization where we could
     * unify these two reads to reduce I/O operations by half.
     */
    TEST(exifOptimization, measureDoubleExivReadPerformance)
    {
        TestArea ta(TEST_NAME, true);

        // Download a geo-referenced image with EXIF data
        fs::path imagePath = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/master/test-datasets/"
            "drone_dataset_brighton_beach/DJI_0018.JPG",
            "DJI_0018.JPG");

        ASSERT_TRUE(fs::exists(imagePath)) << "Test image not found";

        const int iterations = 10;
        std::vector<double> durations;
        durations.reserve(iterations);

        // Warm up (ensure file is in OS cache)
        {
            Entry warmupEntry;
            parseEntry(imagePath, ta.getFolder(), warmupEntry, false);
        }

        // Measure multiple iterations
        for (int i = 0; i < iterations; ++i)
        {
            Entry entry;

            auto start = std::chrono::high_resolution_clock::now();
            parseEntry(imagePath, ta.getFolder(), entry, false);
            auto end = std::chrono::high_resolution_clock::now();

            double durationMs = std::chrono::duration<double, std::milli>(end - start).count();
            durations.push_back(durationMs);

            // Verify EXIF data was correctly extracted (should be a GeoImage with metadata)
            EXPECT_EQ(entry.type, EntryType::GeoImage) << "Entry should be detected as GeoImage";
            EXPECT_TRUE(entry.properties.contains("width")) << "Width should be extracted";
            EXPECT_TRUE(entry.properties.contains("height")) << "Height should be extracted";
            EXPECT_TRUE(entry.properties.contains("make")) << "Camera make should be extracted";
            EXPECT_TRUE(entry.properties.contains("model")) << "Camera model should be extracted";
            EXPECT_TRUE(entry.properties.contains("captureTime")) << "Capture time should be extracted";
            EXPECT_FALSE(entry.point_geom.empty()) << "GPS coordinates should be extracted";
        }

        // Calculate statistics
        double sum = std::accumulate(durations.begin(), durations.end(), 0.0);
        double mean = sum / iterations;

        double sqSum = 0.0;
        for (double d : durations)
        {
            sqSum += (d - mean) * (d - mean);
        }
        double stdDev = std::sqrt(sqSum / iterations);

        double minDuration = *std::min_element(durations.begin(), durations.end());
        double maxDuration = *std::max_element(durations.begin(), durations.end());

        std::cout << "\n=== EXIF Double Read Performance Test ===" << std::endl;
        std::cout << "Iterations: " << iterations << std::endl;
        std::cout << "Mean time: " << mean << " ms" << std::endl;
        std::cout << "Std dev: " << stdDev << " ms" << std::endl;
        std::cout << "Min: " << minDuration << " ms" << std::endl;
        std::cout << "Max: " << maxDuration << " ms" << std::endl;
        std::cout << "==========================================\n" << std::endl;

        // This test establishes a baseline. After optimization, the mean time
        // should be reduced by approximately 30-50% since we eliminate one
        // Exiv2::ImageFactory::open() + readMetadata() call.
    }

    /**
     * @brief Test parsing multiple images to simulate real-world usage.
     *
     * This measures the cumulative overhead of double EXIF reading
     * across multiple images, which is common in drone image sets.
     */
    TEST(exifOptimization, measureBatchPerformance)
    {
        TestArea ta(TEST_NAME, true);

        // Download multiple images
        std::vector<fs::path> images;

        fs::path img1 = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/master/test-datasets/"
            "drone_dataset_brighton_beach/DJI_0018.JPG",
            "DJI_0018.JPG");
        images.push_back(img1);

        fs::path img2 = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/master/test-datasets/"
            "drone_dataset_brighton_beach/DJI_0019.JPG",
            "DJI_0019.JPG");
        images.push_back(img2);

        fs::path img3 = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/master/test-datasets/"
            "drone_dataset_brighton_beach/DJI_0020.JPG",
            "DJI_0020.JPG");
        images.push_back(img3);

        // Verify all images exist
        for (const auto& imgPath : images)
        {
            ASSERT_TRUE(fs::exists(imgPath)) << "Test image not found: " << imgPath;
        }

        // Warm up
        for (const auto& imgPath : images)
        {
            Entry warmupEntry;
            parseEntry(imgPath, ta.getFolder(), warmupEntry, false);
        }

        const int iterations = 5;
        std::vector<double> batchDurations;
        batchDurations.reserve(iterations);

        for (int i = 0; i < iterations; ++i)
        {
            auto batchStart = std::chrono::high_resolution_clock::now();

            for (const auto& imgPath : images)
            {
                Entry entry;
                parseEntry(imgPath, ta.getFolder(), entry, false);

                // Verify each entry is properly parsed
                EXPECT_EQ(entry.type, EntryType::GeoImage);
                EXPECT_FALSE(entry.point_geom.empty());
            }

            auto batchEnd = std::chrono::high_resolution_clock::now();
            double batchMs = std::chrono::duration<double, std::milli>(batchEnd - batchStart).count();
            batchDurations.push_back(batchMs);
        }

        double batchSum = std::accumulate(batchDurations.begin(), batchDurations.end(), 0.0);
        double batchMean = batchSum / iterations;
        double perImageMean = batchMean / images.size();

        std::cout << "\n=== EXIF Batch Performance Test ===" << std::endl;
        std::cout << "Images per batch: " << images.size() << std::endl;
        std::cout << "Iterations: " << iterations << std::endl;
        std::cout << "Mean batch time: " << batchMean << " ms" << std::endl;
        std::cout << "Mean per-image time: " << perImageMean << " ms" << std::endl;
        std::cout << "=====================================\n" << std::endl;

        // After optimization, we expect batch time to decrease significantly
        // because each image will have only one Exiv2 read instead of two.
    }

    /**
     * @brief Test that verifies fingerprint and parseEntry extract consistent data.
     *
     * This test ensures that if we unify the EXIF reading, the type detection
     * (currently in fingerprint) and metadata extraction remain consistent.
     */
    TEST(exifOptimization, fingerprintAndParseConsistency)
    {
        TestArea ta(TEST_NAME, true);

        fs::path imagePath = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/master/test-datasets/"
            "drone_dataset_brighton_beach/DJI_0018.JPG",
            "DJI_0018.JPG");

        ASSERT_TRUE(fs::exists(imagePath)) << "Test image not found";

        // Get type from fingerprint directly
        EntryType fingerprintType = fingerprint(imagePath);

        // Parse entry (which calls fingerprint internally)
        Entry entry;
        parseEntry(imagePath, ta.getFolder(), entry, false);

        // Verify consistency
        EXPECT_EQ(fingerprintType, entry.type)
            << "fingerprint() and parseEntry() should produce the same EntryType";

        EXPECT_EQ(entry.type, EntryType::GeoImage)
            << "Image with GPS should be GeoImage";

        // Verify essential metadata is present
        EXPECT_GT(entry.properties["width"].get<int>(), 0);
        EXPECT_GT(entry.properties["height"].get<int>(), 0);
        EXPECT_FALSE(entry.properties["make"].get<std::string>().empty());
        EXPECT_FALSE(entry.properties["model"].get<std::string>().empty());
        EXPECT_FALSE(entry.point_geom.empty());

        std::cout << "\n=== Fingerprint/Parse Consistency Test ===" << std::endl;
        std::cout << "Image type: " << typeToHuman(entry.type) << std::endl;
        std::cout << "Dimensions: " << entry.properties["width"] << "x" << entry.properties["height"] << std::endl;
        std::cout << "Camera: " << entry.properties["make"] << " " << entry.properties["model"] << std::endl;
        std::cout << "GPS: " << entry.point_geom.toWkt() << std::endl;
        std::cout << "==========================================\n" << std::endl;
    }

    /**
     * @brief Test non-geo image to verify type detection works correctly.
     *
     * Tests that images without GPS data are correctly identified as Image (not GeoImage).
     */
    TEST(exifOptimization, nonGeoImageTypeDetection)
    {
        TestArea ta(TEST_NAME, true);

        // Download or create a test image without GPS data
        // Using a PNG which typically doesn't have EXIF GPS data
        fs::path imagePath = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/master/images/test.png",
            "test.png", true);

        if (!fs::exists(imagePath))
        {
            GTEST_SKIP() << "Test image not available";
        }

        Entry entry;
        parseEntry(imagePath, ta.getFolder(), entry, false);

        // PNG without GPS should be a plain Image
        EXPECT_EQ(entry.type, EntryType::Image)
            << "PNG without GPS should be detected as Image";

        // GPS should be empty
        EXPECT_TRUE(entry.point_geom.empty())
            << "Image without GPS should have empty point geometry";

        std::cout << "\n=== Non-Geo Image Test ===" << std::endl;
        std::cout << "Image type: " << typeToHuman(entry.type) << std::endl;
        std::cout << "Has GPS: " << (!entry.point_geom.empty() ? "Yes" : "No") << std::endl;
        std::cout << "===========================\n" << std::endl;
    }
}
