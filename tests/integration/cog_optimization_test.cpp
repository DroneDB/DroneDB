/* Test for COG optimization functionality */
#include "gtest/gtest.h"
#include "cog_utils.h"
#include "cog.h"
#include "logger.h"
#include "testarea.h"
#include "ddb.h"
#include <chrono>

namespace {

class CogOptimizationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Initialize DDB process
    DDBRegisterProcess(false);
  }
};

TEST_F(CogOptimizationTest, TestCogDetection) {
    TestArea ta("CogOptimizationTest");

    // Download test files from DroneDB test data repository
    fs::path cogFile = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/master/ortho/brighton-beach-cog.tif",
        "brighton-beach-cog.tif"
    );

    fs::path nonCogFile = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/master/ortho/brighton-beach.tif",
        "brighton-beach.tif"
    );

    // Test COG detection
    EXPECT_TRUE(ddb::isOptimizedCog(cogFile.string()))
        << "COG file should be detected as optimized";

    EXPECT_FALSE(ddb::isOptimizedCog(nonCogFile.string()))
        << "Non-COG file should not be detected as optimized";
}

TEST_F(CogOptimizationTest, TestBuildCogOptimization) {
    TestArea ta("CogOptimizationTest");

    // Download COG test file
    fs::path cogFile = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/master/ortho/brighton-beach-cog.tif",
        "brighton-beach-cog.tif"
    );

    // Test building COG from already optimized file
    fs::path outputPath = ta.getPath("test-output-cog.tif");

    auto startTime = std::chrono::high_resolution_clock::now();

    EXPECT_NO_THROW({
        ddb::buildCog(cogFile.string(), outputPath.string());
    });

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

    // Verify output file exists
    EXPECT_TRUE(fs::exists(outputPath)) << "Output COG file should exist";

    // The optimized copy should be very fast (less than 5 seconds for test files)
    EXPECT_LT(duration.count(), 5000) << "COG copy should be fast, took " << duration.count() << "ms";

    // Verify output is also a valid COG
    EXPECT_TRUE(ddb::isOptimizedCog(outputPath.string()))
        << "Output should also be detected as optimized COG";
}

TEST_F(CogOptimizationTest, TestBuildCogFromNonOptimized) {
    TestArea ta("CogOptimizationTest");

    // Download non-COG test file
    fs::path nonCogFile = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/master/ortho/brighton-beach.tif",
        "brighton-beach.tif"
    );

    // Test building COG from non-optimized file (should do full rebuild)
    fs::path outputPath = ta.getPath("test-output-from-noncog.tif");

    EXPECT_NO_THROW({
        ddb::buildCog(nonCogFile.string(), outputPath.string());
    });

    // Verify output file exists
    EXPECT_TRUE(fs::exists(outputPath)) << "Output COG file should exist";

    // Verify output is a valid COG (after full rebuild)
    EXPECT_TRUE(ddb::isOptimizedCog(outputPath.string()))
        << "Output from non-COG should be optimized after rebuild";
}

TEST_F(CogOptimizationTest, TestInvalidFiles) {
    // Test with non-existent file
    EXPECT_FALSE(ddb::isOptimizedCog("non_existent_file.tif"))
        << "Non-existent file should return false";

    // Test buildCog with invalid input
    EXPECT_THROW({
        ddb::buildCog("non_existent_input.tif", "output.tif");
    }, std::exception) << "Should throw exception for non-existent input";
}

} // namespace