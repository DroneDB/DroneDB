/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "build.h"
#include "buildlock.h"
#include "dbops.h"
#include "exceptions.h"
#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

#include <chrono>
#include <fstream>
#include <thread>
#include <atomic>

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

        // Download real test files
        orthoPath = testArea->downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/master/brighton/odm_orthophoto.tif",
            "ortho.tif");

        pointCloudPath = testArea->downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/master/brighton/point_cloud.laz",
            "point_cloud.laz");

        // Create a non-buildable test file
        testNonBuildablePath = testArea->getPath("test_text.txt");
        std::ofstream(testNonBuildablePath) << "test content";

        // Add real files to the database index
        ddb::addToIndex(db.get(), {orthoPath.string(), pointCloudPath.string()});
    }

    void TearDown() override {
        db.reset();
        testArea.reset();
    }

    std::unique_ptr<TestArea> testArea;
    fs::path dbPath;
    std::unique_ptr<Database> db;
    fs::path orthoPath;
    fs::path pointCloudPath;
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
    // Test with a buildable ortho file but no active build
    EXPECT_FALSE(ddb::isBuildActive(db.get(), orthoPath.string()));

    // Test with a buildable point cloud file but no active build
    EXPECT_FALSE(ddb::isBuildActive(db.get(), pointCloudPath.string()));
}

TEST_F(IsBuildActiveTest, OrthoFileWithActiveBuild) {
    // Test with an ortho file that has an active build
    Entry orthoEntry;
    fs::path relativePath = fs::relative(orthoPath, dbPath);
    EXPECT_TRUE(ddb::getEntry(db.get(), relativePath.string(), orthoEntry));

    // Construct build path for ortho COG (Cloud Optimized GeoTIFF) - ensure directory exists
    std::string buildPath = db->buildDirectory().string();
    fs::path orthoOutputPath = fs::path(buildPath) / orthoEntry.hash / "cog";
    fs::create_directories(orthoOutputPath.parent_path());

    // Create a build lock to simulate active build
    {
        BuildLock activeBuild(orthoOutputPath.string());
        EXPECT_TRUE(activeBuild.isHolding());

        // Now isBuildActive should return true
        EXPECT_TRUE(ddb::isBuildActive(db.get(), relativePath.string()));
    }

    // After lock is released, should return false
    EXPECT_FALSE(ddb::isBuildActive(db.get(), relativePath.string()));
}

TEST_F(IsBuildActiveTest, PointCloudFileWithActiveBuild) {
    // Test with a point cloud file that has an active build
    Entry pcEntry;
    fs::path relativePath = fs::relative(pointCloudPath, dbPath);
    EXPECT_TRUE(ddb::getEntry(db.get(), relativePath.string(), pcEntry));

    // Construct build path for point cloud (EPT) - ensure directory exists
    std::string buildPath = db->buildDirectory().string();
    fs::path pcOutputPath = fs::path(buildPath) / pcEntry.hash / "ept";
    fs::create_directories(pcOutputPath.parent_path());

    // Create a build lock to simulate active build
    {
        BuildLock activeBuild(pcOutputPath.string());
        EXPECT_TRUE(activeBuild.isHolding());

        // Now isBuildActive should return true
        EXPECT_TRUE(ddb::isBuildActive(db.get(), relativePath.string()));
    }

    // After lock is released, should return false
    EXPECT_FALSE(ddb::isBuildActive(db.get(), relativePath.string()));
}

TEST_F(IsBuildActiveTest, SimpleLockTest) {
    // Simple test to verify build lock mechanism works with real files
    fs::path testOutputPath = testArea->getPath("test_build");

    // Test that no build is active initially
    {
        BuildLock testLock(testOutputPath.string(), false); // Try without waiting
        EXPECT_TRUE(testLock.isHolding());
        testLock.release();
    }

    // Test concurrent access is blocked
    {
        BuildLock firstLock(testOutputPath.string());
        EXPECT_TRUE(firstLock.isHolding());

        EXPECT_THROW({
            BuildLock secondLock(testOutputPath.string(), false);
        }, AppException);
    }
}

TEST_F(IsBuildActiveTest, RealBuildInThread) {
    // Test with a real build running in a separate thread
    Entry orthoEntry;
    fs::path relativePath = fs::relative(orthoPath, dbPath);
    EXPECT_TRUE(ddb::getEntry(db.get(), relativePath.string(), orthoEntry));

    // Initially no build should be active
    EXPECT_FALSE(ddb::isBuildActive(db.get(), relativePath.string()));

    // Create output directory for the build - use the database build directory
    fs::path buildOutputPath = db->buildDirectory();
    fs::create_directories(buildOutputPath);

    // Flag to control and monitor the build thread
    std::atomic<bool> buildStarted{false};
    std::atomic<bool> buildCompleted{false};
    std::exception_ptr buildException = nullptr;

    // Start build in a separate thread
    std::thread buildThread([&]() {
        try {
            buildStarted = true;

            // Add a small delay to ensure the build is detected
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // Run the actual build - this will create the build lock internally
            ddb::build(db.get(), relativePath.string(), buildOutputPath.string());

            buildCompleted = true;
        } catch (...) {
            buildException = std::current_exception();
            buildCompleted = true;
        }
    });

    // Wait for build to start
    while (!buildStarted) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Give the build some time to acquire the lock and start processing
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Now isBuildActive should return true (build is running)
    bool isActive = ddb::isBuildActive(db.get(), relativePath.string());

    EXPECT_TRUE(isActive || buildCompleted) << "Build should be active or already completed";

    // Wait for build to complete
    buildThread.join();

    // Check if there was an exception in the build thread
    if (buildException) {
        try {
            std::rethrow_exception(buildException);
        } catch (const std::exception& e) {
            // Build might fail for various reasons (missing dependencies, etc.)
            // but we can still test if isBuildActive detected it correctly
            GTEST_LOG_(INFO) << "Build failed with: " << e.what();
        }
    }

    // Wait a bit more to ensure lock is released
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // After build completes, isBuildActive should return false
    EXPECT_FALSE(ddb::isBuildActive(db.get(), relativePath.string()));

    // The key test is that we detected the build while it was running
    // Even if the build failed, we should have detected it was active
    EXPECT_TRUE(isActive) << "isBuildActive should have detected the running build";
}

} // anonymous namespace