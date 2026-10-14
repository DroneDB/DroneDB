/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "build.h"
#include "dbops.h"
#include "ddb.h"
#include "exceptions.h"
#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

#include <chrono>
#include <fstream>

namespace {

using namespace ddb;

// Test suite for isBuildComplete / DDBIsBuildComplete
class IsBuildCompleteTest : public ::testing::Test {
protected:
    void SetUp() override {
        testArea = std::make_unique<TestArea>(
            "IsBuildCompleteTest-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        dbPath = testArea->getPath("");
        ddb::initIndex(dbPath.string());
        db = ddb::open(dbPath.string(), true);

        // Download real, type-detectable test assets so getEntry() returns the
        // proper EntryType (Vector / PointCloud / GeoRaster / Model).
        vectorPath = testArea->downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/test2.geojson",
            "test.geojson");

        orthoPath = testArea->downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/master/brighton/odm_orthophoto.tif",
            "ortho.tif");

        pointCloudPath = testArea->downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/master/brighton/point_cloud.laz",
            "point_cloud.laz");

        // A non-buildable plain text file
        nonBuildablePath = testArea->getPath("readme.txt");
        std::ofstream(nonBuildablePath) << "test content";

        ddb::addToIndex(db.get(),
                        {vectorPath.string(), orthoPath.string(), pointCloudPath.string(),
                         nonBuildablePath.string()});
    }

    void TearDown() override {
        db.reset();
        testArea.reset();
    }

    // Resolve the hash of an indexed entry by relative path.
    std::string hashOf(const fs::path& absolutePath) {
        Entry e;
        const auto rel = fs::relative(absolutePath, dbPath);
        EXPECT_TRUE(ddb::getEntry(db.get(), rel.string(), e));
        return e.hash;
    }

    // Convenience: create a file with the given content (defaults non-empty).
    static void writeFile(const fs::path& p, const std::string& content = "x") {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << content;
    }

    std::unique_ptr<TestArea> testArea;
    fs::path dbPath;
    std::unique_ptr<Database> db;
    fs::path vectorPath;
    fs::path orthoPath;
    fs::path pointCloudPath;
    fs::path nonBuildablePath;
};

TEST_F(IsBuildCompleteTest, NonExistentEntryReturnsFalse) {
    EXPECT_FALSE(ddb::isBuildComplete(db.get(), "no_such_file.tif"));
}

TEST_F(IsBuildCompleteTest, NonBuildableEntryReturnsFalse) {
    const auto rel = fs::relative(nonBuildablePath, dbPath).string();
    EXPECT_FALSE(ddb::isBuildComplete(db.get(), rel));
}

TEST_F(IsBuildCompleteTest, NoArtifactsReturnsFalse) {
    const auto rel = fs::relative(orthoPath, dbPath).string();
    EXPECT_FALSE(ddb::isBuildComplete(db.get(), rel));
}

// --- Generic types: copc/cog/nxs ---

TEST_F(IsBuildCompleteTest, RasterEmptyFolderReturnsFalse) {
    const auto hash = hashOf(orthoPath);
    const fs::path out = fs::path(db->buildDirectory()) / hash / "cog";
    fs::create_directories(out);  // empty folder

    const auto rel = fs::relative(orthoPath, dbPath).string();
    EXPECT_FALSE(ddb::isBuildComplete(db.get(), rel));
}

TEST_F(IsBuildCompleteTest, RasterFolderWithEmptyFileReturnsFalse) {
    const auto hash = hashOf(orthoPath);
    const fs::path out = fs::path(db->buildDirectory()) / hash / "cog";
    writeFile(out / "cog.tif", "");  // zero-byte file

    const auto rel = fs::relative(orthoPath, dbPath).string();
    EXPECT_FALSE(ddb::isBuildComplete(db.get(), rel));
}

TEST_F(IsBuildCompleteTest, RasterFolderWithContentReturnsTrue) {
    const auto hash = hashOf(orthoPath);
    const fs::path out = fs::path(db->buildDirectory()) / hash / "cog";
    writeFile(out / "cog.tif", "some COG bytes");

    const auto rel = fs::relative(orthoPath, dbPath).string();
    EXPECT_TRUE(ddb::isBuildComplete(db.get(), rel));
}

TEST_F(IsBuildCompleteTest, PointCloudFolderWithContentReturnsTrue) {
    const auto hash = hashOf(pointCloudPath);
    const fs::path out = fs::path(db->buildDirectory()) / hash / "copc";
    writeFile(out / "point_cloud.copc.laz", "some COPC bytes");

    const auto rel = fs::relative(pointCloudPath, dbPath).string();
    EXPECT_TRUE(ddb::isBuildComplete(db.get(), rel));
}

TEST_F(IsBuildCompleteTest, PointCloudWithNestedContentReturnsTrue) {
    // Recursive scan must traverse subdirectories
    const auto hash = hashOf(pointCloudPath);
    const fs::path out = fs::path(db->buildDirectory()) / hash / "copc";
    writeFile(out / "nested" / "deeper" / "file.bin", "deep content");

    const auto rel = fs::relative(pointCloudPath, dbPath).string();
    EXPECT_TRUE(ddb::isBuildComplete(db.get(), rel));
}

// --- Vector: requires BOTH source.gpkg AND mvt/metadata.json non-empty ---

TEST_F(IsBuildCompleteTest, VectorMissingMvtReturnsFalse) {
    const auto hash = hashOf(vectorPath);
    const fs::path base = fs::path(db->buildDirectory()) / hash;
    writeFile(base / "vec" / "source.gpkg", "gpkg bytes");
    // No mvt/metadata.json

    const auto rel = fs::relative(vectorPath, dbPath).string();
    EXPECT_FALSE(ddb::isBuildComplete(db.get(), rel));
}

TEST_F(IsBuildCompleteTest, VectorMissingGpkgReturnsFalse) {
    const auto hash = hashOf(vectorPath);
    const fs::path base = fs::path(db->buildDirectory()) / hash;
    writeFile(base / "mvt" / "metadata.json", "{}");
    // No vec/source.gpkg

    const auto rel = fs::relative(vectorPath, dbPath).string();
    EXPECT_FALSE(ddb::isBuildComplete(db.get(), rel));
}

TEST_F(IsBuildCompleteTest, VectorWithLegacyFgbOnlyReturnsFalse) {
    // Pre-migration installations have vec/vector.fgb but neither source.gpkg
    // nor mvt/. This is exactly the case the artifact checker must detect.
    const auto hash = hashOf(vectorPath);
    const fs::path base = fs::path(db->buildDirectory()) / hash;
    writeFile(base / "vec" / "vector.fgb", "legacy fgb bytes");

    const auto rel = fs::relative(vectorPath, dbPath).string();
    EXPECT_FALSE(ddb::isBuildComplete(db.get(), rel));
}

TEST_F(IsBuildCompleteTest, VectorWithEmptyMvtMetadataReturnsFalse) {
    const auto hash = hashOf(vectorPath);
    const fs::path base = fs::path(db->buildDirectory()) / hash;
    writeFile(base / "vec" / "source.gpkg", "gpkg bytes");
    writeFile(base / "mvt" / "metadata.json", "");  // zero-byte

    const auto rel = fs::relative(vectorPath, dbPath).string();
    EXPECT_FALSE(ddb::isBuildComplete(db.get(), rel));
}

TEST_F(IsBuildCompleteTest, VectorWithBothArtifactsReturnsTrue) {
    const auto hash = hashOf(vectorPath);
    const fs::path base = fs::path(db->buildDirectory()) / hash;
    writeFile(base / "vec" / "source.gpkg", "gpkg bytes");
    writeFile(base / "mvt" / "metadata.json", "{\"name\":\"layer\"}");

    const auto rel = fs::relative(vectorPath, dbPath).string();
    EXPECT_TRUE(ddb::isBuildComplete(db.get(), rel));
}

// --- C API ---

// --- Model: tiles are optional, except an identifiable interrupted swap ---

// Indexes a dependency-free OBJ and returns its relative path.
std::string addModel(Database* db, const fs::path& dbPath) {
    const fs::path obj = dbPath / "model.obj";
    std::ofstream(obj) << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    ddb::addToIndex(db, {obj.string()});
    Entry e;
    EXPECT_TRUE(ddb::getEntry(db, "model.obj", e));
    EXPECT_EQ(e.type, EntryType::Model);
    return "model.obj";
}

TEST_F(IsBuildCompleteTest, ModelWithoutArtifactsReturnsFalse) {
    const auto rel = addModel(db.get(), dbPath);
    EXPECT_FALSE(ddb::isBuildComplete(db.get(), rel));
}

TEST_F(IsBuildCompleteTest, ModelEmptyNxsReturnsFalse) {
    const auto rel = addModel(db.get(), dbPath);
    const fs::path base = fs::path(db->buildDirectory()) / hashOf(dbPath / rel);
    writeFile(base / "nxs" / "model.nxz", "");
    EXPECT_FALSE(ddb::isBuildComplete(db.get(), rel));
}

TEST_F(IsBuildCompleteTest, ModelTilesOnlyReturnsFalse) {
    const auto rel = addModel(db.get(), dbPath);
    const fs::path base = fs::path(db->buildDirectory()) / hashOf(dbPath / rel);
    writeFile(base / "3dtiles" / "tileset.json", "{}");
    EXPECT_FALSE(ddb::isBuildComplete(db.get(), rel));
}

TEST_F(IsBuildCompleteTest, ModelNxsWithoutTilesReturnsTrue) {
    const auto rel = addModel(db.get(), dbPath);
    const fs::path base = fs::path(db->buildDirectory()) / hashOf(dbPath / rel);
    writeFile(base / "nxs" / "model.nxz", "some NXZ bytes");
    EXPECT_TRUE(ddb::isBuildComplete(db.get(), rel));

    writeFile(base / "3dtiles" / "tileset.json", "{}");
    EXPECT_TRUE(ddb::isBuildComplete(db.get(), rel));
}

TEST(isBuildComplete, modelSwapExceptionIsNarrowAndReadOnly) {
    TestArea ta(TEST_NAME, true);
    const fs::path root = ta.getFolder();
    initIndex(root.string());
    auto db = ddb::open(root.string(), true);
    const auto rel = addModel(db.get(), root);
    Entry entry;
    ASSERT_TRUE(getEntry(db.get(), rel, entry));
    const fs::path base = db->buildDirectory() / entry.hash;
    const fs::path tiles = base / "3dtiles";
    const fs::path old = base / "3dtiles-old";
    fs::create_directories(base / "nxs");
    std::ofstream(base / "nxs" / "model.nxz") << "existing Nexus";
    const auto check = [&](bool expected) {
        bool complete = !expected;
        ASSERT_EQ(DDBIsBuildComplete(root.string().c_str(), rel.c_str(), &complete), DDBERR_NONE);
        EXPECT_EQ(complete, expected);
        EXPECT_FALSE(fs::exists(tiles.string() + ".building"));
    };
    check(true); // no optional tiles at all
    fs::create_directory(old);
    std::ofstream(old / "tileset.json");
    check(true); // empty backup cannot identify a recoverable swap
    std::ofstream(old / "tileset.json") << "{}";
    check(false);
    EXPECT_TRUE(fs::exists(old / "tileset.json"));
    EXPECT_FALSE(fs::exists(tiles));
    fs::create_directory(tiles);
    std::ofstream(tiles / "tileset.json");
    check(false);
    EXPECT_EQ(fs::file_size(tiles / "tileset.json"), 0u);
    std::ofstream(tiles / "tileset.json") << "{}";
    check(true); // both usable: ordinary completeness is restored
    EXPECT_TRUE(fs::exists(old / "tileset.json")); // no cleanup during observation
}

TEST_F(IsBuildCompleteTest, DDBIsBuildCompleteCAPI) {
    const auto hash = hashOf(orthoPath);
    const fs::path out = fs::path(db->buildDirectory()) / hash / "cog";
    writeFile(out / "cog.tif", "content");

    const auto rel = fs::relative(orthoPath, dbPath).string();

    bool complete = false;
    EXPECT_EQ(DDBIsBuildComplete(dbPath.string().c_str(), rel.c_str(), &complete), DDBERR_NONE);
    EXPECT_TRUE(complete);

    // Validate argument checks
    EXPECT_EQ(DDBIsBuildComplete(nullptr, rel.c_str(), &complete), DDBERR_EXCEPTION);
    EXPECT_EQ(DDBIsBuildComplete(dbPath.string().c_str(), nullptr, &complete), DDBERR_EXCEPTION);
    EXPECT_EQ(DDBIsBuildComplete(dbPath.string().c_str(), rel.c_str(), nullptr), DDBERR_EXCEPTION);
}

}  // namespace
