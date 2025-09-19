/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "build.h"
#include "dbops.h"
#include "exceptions.h"
#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

#include <chrono>
#include <fstream>

namespace {

using namespace ddb;

// Test suite for isBuildActive function
class IsBuildActiveTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a unique test area for each test
        testArea = std::make_unique<TestArea>("IsBuildActiveTest-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        // Create and initialize a test database
        dbPath = testArea->getPath("");  // Use the directory, not a file
        ddb::initIndex(dbPath.string());
        db = ddb::open(dbPath.string(), true);

        // Create test files for testing
        testImagePath = testArea->getPath("test_image.jpg");
        testRasterPath = testArea->getPath("test_raster.tif");
        testNonBuildablePath = testArea->getPath("test_text.txt");

        // Create actual test files
        std::ofstream(testImagePath).close();
        std::ofstream(testRasterPath).close();
        std::ofstream(testNonBuildablePath) << "test content";
    }

    void TearDown() override {
        db.reset();
        testArea.reset();
    }

    std::unique_ptr<TestArea> testArea;
    fs::path dbPath;
    std::unique_ptr<Database> db;
    fs::path testImagePath;
    fs::path testRasterPath;
    fs::path testNonBuildablePath;
};

TEST_F(IsBuildActiveTest, NonExistentFile) {
    // Test with a file that doesn't exist in the database
    EXPECT_FALSE(ddb::isBuildActive(db.get(), "non_existent_file.tif"));
}

TEST_F(IsBuildActiveTest, ValidDatabaseConnection) {
    // Test that the database connection works
    EXPECT_TRUE(db != nullptr);
}

TEST_F(IsBuildActiveTest, NonBuildableFile) {
    // Test with a non-buildable file (should return false)
    EXPECT_FALSE(ddb::isBuildActive(db.get(), testNonBuildablePath.string()));
}

TEST_F(IsBuildActiveTest, BuildableFileNoBuildActive) {
    // Test with a buildable file but no active build
    EXPECT_FALSE(ddb::isBuildActive(db.get(), testRasterPath.string()));
}

TEST_F(IsBuildActiveTest, BuildableFileWithActiveBuild) {
    // Test with a buildable file that has an active build

    // First we need to add the file to the database and make it buildable
    // This would typically be done through the normal DDB workflow

    // For now, just test that the function doesn't crash with a valid path
    EXPECT_NO_THROW(ddb::isBuildActive(db.get(), testRasterPath.string()));
}

} // anonymous namespace