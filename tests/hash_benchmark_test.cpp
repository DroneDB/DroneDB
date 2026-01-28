/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <chrono>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <vector>

#include "gtest/gtest.h"
#include "hash.h"
#include "mio.h"
#include "test.h"
#include "testarea.h"

// OpenSSL for comparison
#include <openssl/evp.h>

namespace {

using namespace ddb;

/**
 * @brief Helper to compute SHA256 using OpenSSL EVP API directly (for validation)
 */
std::string opensslSHA256(const char* data, size_t size) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return "";

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
        EVP_MD_CTX_free(ctx);
        return "";
    }

    EVP_DigestUpdate(ctx, data, size);

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;
    EVP_DigestFinal_ex(ctx, hash, &hash_len);
    EVP_MD_CTX_free(ctx);

    std::ostringstream oss;
    for (unsigned int i = 0; i < hash_len; ++i) {
        oss << std::hex << std::setfill('0') << std::setw(2) << (int)hash[i];
    }
    return oss.str();
}

/**
 * @brief Benchmark for hashing a single large file (~54 MB orthophoto)
 *
 * This test downloads a large orthophoto and measures the time to compute
 * its SHA256 hash. Run multiple iterations to get stable measurements.
 *
 * Usage: Run with --gtest_also_run_disabled_tests to execute this benchmark
 */
MANUAL_TEST(hashBenchmark, largeFile) {
    TestArea ta(TEST_NAME);

    // Download large orthophoto (~54 MB)
    fs::path largeFile = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/refs/heads/master/ortho/aukerman.tif",
        "aukerman.tif");

    ASSERT_TRUE(fs::exists(largeFile)) << "Failed to download test file";

    const auto fileSize = fs::file_size(largeFile);
    std::cout << "\n=== Large File Hash Benchmark ===" << std::endl;
    std::cout << "File: " << largeFile.filename().string() << std::endl;
    std::cout << "Size: " << std::fixed << std::setprecision(2)
              << (fileSize / (1024.0 * 1024.0)) << " MB" << std::endl;

    const int warmupRuns = 2;
    const int benchmarkRuns = 5;
    std::vector<double> timings;
    std::string hash;

    // Warmup runs (to prime disk cache)
    std::cout << "\nWarmup runs: " << warmupRuns << std::endl;
    for (int i = 0; i < warmupRuns; ++i) {
        hash = Hash::fileSHA256(largeFile.string());
    }
    std::cout << "SHA256: " << hash << std::endl;

    // Benchmark runs
    std::cout << "\nBenchmark runs: " << benchmarkRuns << std::endl;
    for (int i = 0; i < benchmarkRuns; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        hash = Hash::fileSHA256(largeFile.string());
        auto end = std::chrono::high_resolution_clock::now();

        std::chrono::duration<double> elapsed = end - start;
        timings.push_back(elapsed.count());

        std::cout << "  Run " << (i + 1) << ": " << std::fixed << std::setprecision(3)
                  << elapsed.count() << " s" << std::endl;
    }

    // Calculate statistics
    double sum = std::accumulate(timings.begin(), timings.end(), 0.0);
    double mean = sum / timings.size();
    double minTime = *std::min_element(timings.begin(), timings.end());
    double maxTime = *std::max_element(timings.begin(), timings.end());

    double throughputMBps = (fileSize / (1024.0 * 1024.0)) / mean;

    std::cout << "\n--- Results ---" << std::endl;
    std::cout << "Average time: " << std::fixed << std::setprecision(3) << mean << " s" << std::endl;
    std::cout << "Min time:     " << std::fixed << std::setprecision(3) << minTime << " s" << std::endl;
    std::cout << "Max time:     " << std::fixed << std::setprecision(3) << maxTime << " s" << std::endl;
    std::cout << "Throughput:   " << std::fixed << std::setprecision(2) << throughputMBps << " MB/s" << std::endl;
    std::cout << "================================\n" << std::endl;

    EXPECT_GT(throughputMBps, 0) << "Throughput should be positive";
}

/**
 * @brief Benchmark for hashing many image files (18 drone images, ~5-8 MB each)
 *
 * This test downloads multiple drone images and measures the total time
 * to compute SHA256 hashes for all of them. This simulates the typical
 * use case of indexing a folder of drone images.
 *
 * Usage: Run with --gtest_also_run_disabled_tests to execute this benchmark
 */
MANUAL_TEST(hashBenchmark, manyImageFiles) {
    TestArea ta(TEST_NAME);

    // List of drone images from brighton beach dataset
    const std::vector<std::string> imageNames = {
        "DJI_0018.JPG", "DJI_0019.JPG", "DJI_0020.JPG", "DJI_0021.JPG",
        "DJI_0022.JPG", "DJI_0023.JPG", "DJI_0024.JPG", "DJI_0025.JPG",
        "DJI_0026.JPG", "DJI_0027.JPG", "DJI_0028.JPG", "DJI_0029.JPG",
        "DJI_0030.JPG", "DJI_0031.JPG", "DJI_0032.JPG", "DJI_0033.JPG",
        "DJI_0034.JPG", "DJI_0035.JPG"
    };

    std::cout << "\n=== Multiple Image Files Hash Benchmark ===" << std::endl;
    std::cout << "Downloading " << imageNames.size() << " drone images..." << std::endl;

    // Download all images
    std::vector<fs::path> imagePaths;
    uint64_t totalSize = 0;

    for (const auto& imageName : imageNames) {
        std::string url = "https://github.com/DroneDB/test_data/raw/refs/heads/master/"
                          "test-datasets/drone_dataset_brighton_beach/" + imageName;
        fs::path imagePath = ta.downloadTestAsset(url, imageName);
        ASSERT_TRUE(fs::exists(imagePath)) << "Failed to download " << imageName;
        imagePaths.push_back(imagePath);
        totalSize += fs::file_size(imagePath);
    }

    std::cout << "Total files: " << imagePaths.size() << std::endl;
    std::cout << "Total size:  " << std::fixed << std::setprecision(2)
              << (totalSize / (1024.0 * 1024.0)) << " MB" << std::endl;
    std::cout << "Avg file:    " << std::fixed << std::setprecision(2)
              << (totalSize / (1024.0 * 1024.0 * imagePaths.size())) << " MB" << std::endl;

    const int warmupRuns = 1;
    const int benchmarkRuns = 3;
    std::vector<double> timings;

    // Warmup run
    std::cout << "\nWarmup runs: " << warmupRuns << std::endl;
    for (int w = 0; w < warmupRuns; ++w) {
        for (const auto& path : imagePaths) {
            Hash::fileSHA256(path.string());
        }
    }

    // Show hashes for verification
    std::cout << "\nFirst/last file hashes:" << std::endl;
    std::cout << "  " << imagePaths.front().filename().string() << ": "
              << Hash::fileSHA256(imagePaths.front().string()) << std::endl;
    std::cout << "  " << imagePaths.back().filename().string() << ": "
              << Hash::fileSHA256(imagePaths.back().string()) << std::endl;

    // Benchmark runs
    std::cout << "\nBenchmark runs: " << benchmarkRuns << std::endl;
    for (int i = 0; i < benchmarkRuns; ++i) {
        auto start = std::chrono::high_resolution_clock::now();

        for (const auto& path : imagePaths) {
            Hash::fileSHA256(path.string());
        }

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = end - start;
        timings.push_back(elapsed.count());

        std::cout << "  Run " << (i + 1) << ": " << std::fixed << std::setprecision(3)
                  << elapsed.count() << " s ("
                  << std::setprecision(1) << (imagePaths.size() / elapsed.count())
                  << " files/s)" << std::endl;
    }

    // Calculate statistics
    double sum = std::accumulate(timings.begin(), timings.end(), 0.0);
    double mean = sum / timings.size();
    double minTime = *std::min_element(timings.begin(), timings.end());
    double maxTime = *std::max_element(timings.begin(), timings.end());

    double throughputMBps = (totalSize / (1024.0 * 1024.0)) / mean;
    double filesPerSecond = imagePaths.size() / mean;

    std::cout << "\n--- Results ---" << std::endl;
    std::cout << "Average time:    " << std::fixed << std::setprecision(3) << mean << " s" << std::endl;
    std::cout << "Min time:        " << std::fixed << std::setprecision(3) << minTime << " s" << std::endl;
    std::cout << "Max time:        " << std::fixed << std::setprecision(3) << maxTime << " s" << std::endl;
    std::cout << "Throughput:      " << std::fixed << std::setprecision(2) << throughputMBps << " MB/s" << std::endl;
    std::cout << "Files/second:    " << std::fixed << std::setprecision(2) << filesPerSecond << std::endl;
    std::cout << "Avg time/file:   " << std::fixed << std::setprecision(3)
              << (mean / imagePaths.size() * 1000) << " ms" << std::endl;
    std::cout << "=============================================\n" << std::endl;

    EXPECT_GT(throughputMBps, 0) << "Throughput should be positive";
    EXPECT_GT(filesPerSecond, 0) << "Files per second should be positive";
}

/**
 * @brief CPU-only benchmark for OpenSSL-based Hash::strSHA256
 *
 * This test hashes data already in memory to isolate CPU performance
 * from disk I/O. Tests various data sizes to verify consistent throughput.
 *
 * Usage: Run with --gtest_also_run_disabled_tests to execute this benchmark
 */
MANUAL_TEST(hashBenchmark, opensslThroughput) {
    std::cout << "\n=== CPU Hash Benchmark: OpenSSL SHA256 ===" << std::endl;

    // Test with multiple data sizes to see scaling behavior
    const std::vector<size_t> dataSizes = {
        1 * 1024 * 1024,    // 1 MB  - small file
        10 * 1024 * 1024,   // 10 MB - typical drone image
        50 * 1024 * 1024,   // 50 MB - orthophoto
        100 * 1024 * 1024,  // 100 MB - large file
        256 * 1024 * 1024   // 256 MB - very large file
    };

    const int warmupRuns = 2;
    const int benchmarkRuns = 5;

    // Generate random data once (to avoid affecting timing)
    std::cout << "Generating test data..." << std::endl;
    std::vector<char> maxData(dataSizes.back());
    std::mt19937 rng(42);  // Fixed seed for reproducibility
    std::uniform_int_distribution<int> dist(0, 255);
    for (auto& byte : maxData) {
        byte = static_cast<char>(dist(rng));
    }
    std::cout << "Generated " << (maxData.size() / (1024.0 * 1024.0)) << " MB of random data\n" << std::endl;

    std::cout << std::setw(12) << "Size (MB)"
              << std::setw(18) << "Throughput"
              << std::endl;
    std::cout << std::string(30, '-') << std::endl;

    for (size_t dataSize : dataSizes) {
        const char* data = maxData.data();

        // Create a string from data for strSHA256
        std::string dataStr(data, dataSize);

        // Warmup
        for (int i = 0; i < warmupRuns; ++i) {
            Hash::strSHA256(dataStr);
        }

        // Verify against direct OpenSSL call
        std::string hashResult = Hash::strSHA256(dataStr);
        std::string opensslResult = opensslSHA256(data, dataSize);
        ASSERT_EQ(hashResult, opensslResult)
            << "Hash mismatch for size " << dataSize << "!\n"
            << "Hash::strSHA256: " << hashResult << "\n"
            << "Direct OpenSSL:  " << opensslResult;

        // Benchmark
        std::vector<double> timings;
        for (int i = 0; i < benchmarkRuns; ++i) {
            auto start = std::chrono::high_resolution_clock::now();
            Hash::strSHA256(dataStr);
            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            timings.push_back(elapsed.count());
        }
        double mean = std::accumulate(timings.begin(), timings.end(), 0.0) / timings.size();

        // Calculate throughput
        double sizeMB = dataSize / (1024.0 * 1024.0);
        double throughput = sizeMB / mean;

        std::cout << std::fixed << std::setprecision(0)
                  << std::setw(12) << sizeMB
                  << std::setw(14) << std::setprecision(1) << throughput << " MB/s"
                  << std::endl;
    }

    std::cout << std::string(30, '-') << std::endl;

    // Also test chunked hashing (simulating file reading with ~1MB chunks)
    std::cout << "\n=== Chunked Hashing (simulating 1MB buffer reads) ===" << std::endl;
    const size_t chunkSize = 1024 * 1024;  // 1 MB chunks (like fileSHA256)
    const size_t totalSize = 100 * 1024 * 1024;  // 100 MB total
    const char* data = maxData.data();

    // Benchmark OpenSSL chunked
    std::vector<double> opensslChunkedTimings;
    for (int i = 0; i < benchmarkRuns; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
        for (size_t offset = 0; offset < totalSize; offset += chunkSize) {
            size_t remaining = std::min(chunkSize, totalSize - offset);
            EVP_DigestUpdate(ctx, data + offset, remaining);
        }
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int hash_len;
        EVP_DigestFinal_ex(ctx, hash, &hash_len);
        EVP_MD_CTX_free(ctx);
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = end - start;
        opensslChunkedTimings.push_back(elapsed.count());
    }
    double opensslChunkedMean = std::accumulate(opensslChunkedTimings.begin(),
                                                 opensslChunkedTimings.end(), 0.0)
                                / opensslChunkedTimings.size();

    double totalSizeMB = totalSize / (1024.0 * 1024.0);

    std::cout << "Data size: " << totalSizeMB << " MB in " << (totalSize / chunkSize) << " chunks" << std::endl;
    std::cout << "Throughput: " << std::fixed << std::setprecision(1)
              << (totalSizeMB / opensslChunkedMean) << " MB/s" << std::endl;
    std::cout << "============================================\n" << std::endl;

    EXPECT_TRUE(true);  // Always pass, this is a benchmark
}

}  // namespace
