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
#include "testfs.h"
#include "utils.h"

#include <chrono>
#include <fstream>
#include <future>
#include <mutex>
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
            testDataUrl("brighton/odm_orthophoto.tif"),
            "ortho.tif");

        pointCloudPath = testArea->downloadTestAsset(
            testDataUrl("brighton/point_cloud.laz"),
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
    EXPECT_TRUE(loadEntry(db.get(), relativePath.string(), orthoEntry));

    // Construct build path for ortho COG (Cloud Optimized GeoTIFF) - ensure directory exists
    std::string buildPath = db->buildDirectory().string();
    fs::path orthoOutputPath = fs::path(buildPath) / orthoEntry.hash / "cog";
    fs::create_directories(orthoOutputPath.parent_path());

    // Create a build lock to simulate active build
    {
        BuildLock activeBuild(orthoOutputPath.string());

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
    EXPECT_TRUE(loadEntry(db.get(), relativePath.string(), pcEntry));

    // Construct build path for point cloud (COPC) - ensure directory exists
    std::string buildPath = db->buildDirectory().string();
    fs::path pcOutputPath = fs::path(buildPath) / pcEntry.hash / "copc";
    fs::create_directories(pcOutputPath.parent_path());

    // Create a build lock to simulate active build
    {
        BuildLock activeBuild(pcOutputPath.string());

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
    }

    // Test concurrent access is blocked
    {
        BuildLock firstLock(testOutputPath.string());

        EXPECT_THROW({
            BuildLock secondLock(testOutputPath.string(), false);
        }, AppException);
    }
}

TEST_F(IsBuildActiveTest, RealBuildInThread) {
    // A real build in another thread is seen while it runs: the build reports its progress while it holds
    // its lock, and its first report waits until the test has looked, so the test looks at a running build
    // however fast the build is (no timing)
    Entry orthoEntry;
    fs::path relativePath = fs::relative(orthoPath, dbPath);
    EXPECT_TRUE(loadEntry(db.get(), relativePath.string(), orthoEntry));
    EXPECT_FALSE(ddb::isBuildActive(db.get(), relativePath.string()));

    fs::path buildOutputPath = db->buildDirectory();
    fs::create_directories(buildOutputPath);

    std::promise<void> running, looked;
    std::shared_future<void> lookedDone = looked.get_future().share();
    std::once_flag first;
    BuildOptions options;
    options.progress = [&](double, std::string_view) {
        std::call_once(first, [&] {
            running.set_value();
            lookedDone.wait();
        });
    };
    std::exception_ptr buildException;
    std::thread buildThread([&]() {
        try {
            ddb::build(db.get(), relativePath.string(), buildOutputPath.string(), false, options);
        } catch (...) {
            buildException = std::current_exception();
        }
        // A build that reported nothing must not leave the test waiting
        std::call_once(first, [&] { running.set_value(); });
    });

    running.get_future().wait();
    const bool isActive = ddb::isBuildActive(db.get(), relativePath.string());
    looked.set_value();
    buildThread.join();

    if (buildException)
        std::rethrow_exception(buildException);
    EXPECT_TRUE(isActive) << "isBuildActive should have detected the running build";
    // The build released its lock when it returned
    EXPECT_FALSE(ddb::isBuildActive(db.get(), relativePath.string()));
}

TEST_F(IsBuildActiveTest, StaleLockFileIsCleanedUp) {
    // Test that isBuildActive detects and cleans up stale lock files
    // (lock files left by crashed processes with dead PIDs)
    Entry orthoEntry;
    fs::path relativePath = fs::relative(orthoPath, dbPath);
    ASSERT_TRUE(loadEntry(db.get(), relativePath.string(), orthoEntry));

    // Construct the build lock file path (matches what build.cpp uses)
    std::string buildPath = db->buildDirectory().string();
    fs::path orthoOutputPath = fs::path(buildPath) / orthoEntry.hash / "cog";
    fs::create_directories(orthoOutputPath.parent_path());
    std::string lockFile = orthoOutputPath.string() + ".building";

    // Create a stale lock file with a dead PID (PID 99 should not be running)
    {
        std::ofstream f(lockFile);
        f << "PID: 99\nProcess: Simulated crashed build\n";
    }
    ASSERT_TRUE(fs::exists(lockFile));

    // isBuildActive should detect the stale lock, clean it up, and return false
    EXPECT_FALSE(ddb::isBuildActive(db.get(), relativePath.string()));

    // The stale lock file should have been removed
    EXPECT_FALSE(fs::exists(lockFile));
}

TEST_F(IsBuildActiveTest, ActiveLockFileIsNotCleaned) {
    // Test that isBuildActive does NOT clean up lock files held by active processes
    Entry orthoEntry;
    fs::path relativePath = fs::relative(orthoPath, dbPath);
    ASSERT_TRUE(loadEntry(db.get(), relativePath.string(), orthoEntry));

    std::string buildPath = db->buildDirectory().string();
    fs::path orthoOutputPath = fs::path(buildPath) / orthoEntry.hash / "cog";
    fs::create_directories(orthoOutputPath.parent_path());

    // Acquire a real build lock (simulates an active build by this process)
    {
        BuildLock activeLock(orthoOutputPath.string());

        // isBuildActive should return true - lock is held by an active process
        EXPECT_TRUE(ddb::isBuildActive(db.get(), relativePath.string()));
    }
}

} // anonymous namespace