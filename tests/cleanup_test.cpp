/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "build.h"
#include "dbops.h"
#include "ddb.h"
#include "entry.h"
#include "exceptions.h"
#include "gtest/gtest.h"
#include "json.h"
#include "test.h"
#include "testarea.h"

#include <algorithm>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <process.h>
#define GET_CURRENT_PID() static_cast<long>(_getpid())
#else
#include <unistd.h>
#define GET_CURRENT_PID() static_cast<long>(::getpid())
#endif

namespace
{
    using namespace ddb;

    // Helper: create a fake build subdirectory for the given hash, with a
    // sentinel file inside, and return the directory path.
    fs::path makeFakeBuildDir(const fs::path &buildDir, const std::string &hash)
    {
        fs::path dir = buildDir / hash;
        fs::create_directories(dir / "cog");
        std::ofstream(dir / "cog" / "cog.tif") << "fake-cog-data";
        return dir;
    }

    fs::path makeFakePendingFile(const fs::path &buildDir, const std::string &hash)
    {
        fs::path p = buildDir / (hash + ".pending");
        std::ofstream(p) << "0\n";
        return p;
    }

    // Write a build lock file (.building) carrying a specific PID.
    void writeLockFile(const fs::path &lockFile, long pid)
    {
        std::ofstream(lockFile) << "PID: " << pid << "\nProcess: test\n";
    }

    // Set up an indexed database with two text entries; returns the test area
    // path and (via out-params) the hashes of the indexed entries.
    fs::path setupIndexedDb(TestArea &ta,
                            std::unique_ptr<Database> &db,
                            std::vector<std::string> &outHashes)
    {
        fs::path testFolder = ta.getFolder();
        ddb::initIndex(testFolder.string());
        db = ddb::open(testFolder.string(), true);

        fs::path f1 = testFolder / "a.txt";
        fs::path f2 = testFolder / "b.txt";
        std::ofstream(f1) << "alpha-content";
        std::ofstream(f2) << "beta-content";

        ddb::addToIndex(db.get(), {f1.string(), f2.string()});

        // Read back hashes from DB
        outHashes.clear();
        auto q = db->query(
            "SELECT hash FROM entries WHERE hash IS NOT NULL AND hash <> '' ORDER BY path");
        while (q->fetch())
            outHashes.push_back(q->getText(0));

        return testFolder;
    }

    // ---------- Tests ----------

    TEST(cleanupBuild, NoBuildDirectoryReturnsEmpty)
    {
        TestArea ta(TEST_NAME, true);
        fs::path testFolder = ta.getFolder();
        ddb::initIndex(testFolder.string());
        auto db = ddb::open(testFolder.string(), true);

        // No build directory created yet; should be a no-op.
        auto removed = ddb::cleanupBuild(db.get(), "");
        EXPECT_TRUE(removed.empty());
    }

    TEST(cleanupBuild, EmptyBuildDirectoryReturnsEmpty)
    {
        TestArea ta(TEST_NAME, true);
        fs::path testFolder = ta.getFolder();
        ddb::initIndex(testFolder.string());
        auto db = ddb::open(testFolder.string(), true);

        fs::path buildDir = db->buildDirectory();
        fs::create_directories(buildDir);

        auto removed = ddb::cleanupBuild(db.get(), "");
        EXPECT_TRUE(removed.empty());
        EXPECT_TRUE(fs::exists(buildDir));
    }

    TEST(cleanupBuild, KeepsValidHashes)
    {
        TestArea ta(TEST_NAME, true);
        std::unique_ptr<Database> db;
        std::vector<std::string> hashes;
        setupIndexedDb(ta, db, hashes);
        ASSERT_EQ(hashes.size(), 2u);

        fs::path buildDir = db->buildDirectory();
        fs::create_directories(buildDir);

        // Create build dirs that match real entry hashes
        fs::path d1 = makeFakeBuildDir(buildDir, hashes[0]);
        fs::path d2 = makeFakeBuildDir(buildDir, hashes[1]);

        auto removed = ddb::cleanupBuild(db.get(), "");
        EXPECT_TRUE(removed.empty());
        EXPECT_TRUE(fs::exists(d1));
        EXPECT_TRUE(fs::exists(d2));
    }

    TEST(cleanupBuild, RemovesOrphanDirectory)
    {
        TestArea ta(TEST_NAME, true);
        std::unique_ptr<Database> db;
        std::vector<std::string> hashes;
        setupIndexedDb(ta, db, hashes);

        fs::path buildDir = db->buildDirectory();
        fs::create_directories(buildDir);

        // One valid + one orphan
        fs::path validDir = makeFakeBuildDir(buildDir, hashes[0]);
        fs::path orphanDir = makeFakeBuildDir(
            buildDir, "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef");

        auto removed = ddb::cleanupBuild(db.get(), "");

        ASSERT_EQ(removed.size(), 1u);
        EXPECT_EQ(fs::path(removed[0]).filename(), orphanDir.filename());
        EXPECT_FALSE(fs::exists(orphanDir));
        EXPECT_TRUE(fs::exists(validDir));
    }

    TEST(cleanupBuild, RemovesOrphanPendingFile)
    {
        TestArea ta(TEST_NAME, true);
        std::unique_ptr<Database> db;
        std::vector<std::string> hashes;
        setupIndexedDb(ta, db, hashes);

        fs::path buildDir = db->buildDirectory();
        fs::create_directories(buildDir);

        // Pending file for a real entry: should remain.
        fs::path validPending = makeFakePendingFile(buildDir, hashes[0]);
        // Pending file for an unknown hash: should be removed.
        fs::path orphanPending = makeFakePendingFile(
            buildDir, "0000000000000000000000000000000000000000000000000000000000000000");

        auto removed = ddb::cleanupBuild(db.get(), "");
        ASSERT_EQ(removed.size(), 1u);
        EXPECT_EQ(fs::path(removed[0]).filename(), orphanPending.filename());
        EXPECT_TRUE(fs::exists(validPending));
        EXPECT_FALSE(fs::exists(orphanPending));
    }

    TEST(cleanupBuild, MixedOrphansAndValid)
    {
        TestArea ta(TEST_NAME, true);
        std::unique_ptr<Database> db;
        std::vector<std::string> hashes;
        setupIndexedDb(ta, db, hashes);

        fs::path buildDir = db->buildDirectory();
        fs::create_directories(buildDir);

        fs::path valid = makeFakeBuildDir(buildDir, hashes[0]);
        fs::path orphan1 = makeFakeBuildDir(
            buildDir, "1111111111111111111111111111111111111111111111111111111111111111");
        fs::path orphan2 = makeFakeBuildDir(
            buildDir, "2222222222222222222222222222222222222222222222222222222222222222");
        fs::path validPending = makeFakePendingFile(buildDir, hashes[1]);
        fs::path orphanPending = makeFakePendingFile(
            buildDir, "3333333333333333333333333333333333333333333333333333333333333333");

        auto removed = ddb::cleanupBuild(db.get(), "");
        EXPECT_EQ(removed.size(), 3u);

        EXPECT_TRUE(fs::exists(valid));
        EXPECT_TRUE(fs::exists(validPending));
        EXPECT_FALSE(fs::exists(orphan1));
        EXPECT_FALSE(fs::exists(orphan2));
        EXPECT_FALSE(fs::exists(orphanPending));
    }

    TEST(cleanupBuild, SkipsOrphanWithActiveLock)
    {
        TestArea ta(TEST_NAME, true);
        std::unique_ptr<Database> db;
        std::vector<std::string> hashes;
        setupIndexedDb(ta, db, hashes);

        fs::path buildDir = db->buildDirectory();
        fs::create_directories(buildDir);

        const std::string orphanHash =
            "abcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabca";
        fs::path orphanDir = makeFakeBuildDir(buildDir, orphanHash);

        // Place an active build lock inside (current process PID is alive)
        fs::path lockFile = orphanDir / "cog.building";
        writeLockFile(lockFile, GET_CURRENT_PID());

        auto removed = ddb::cleanupBuild(db.get(), "");

        EXPECT_TRUE(removed.empty());
        EXPECT_TRUE(fs::exists(orphanDir)) << "Active-locked orphan should not be removed";
        EXPECT_TRUE(fs::exists(lockFile));
    }

    TEST(cleanupBuild, RemovesOrphanWithStaleLock)
    {
        TestArea ta(TEST_NAME, true);
        std::unique_ptr<Database> db;
        std::vector<std::string> hashes;
        setupIndexedDb(ta, db, hashes);

        fs::path buildDir = db->buildDirectory();
        fs::create_directories(buildDir);

        const std::string orphanHash =
            "fefefefefefefefefefefefefefefefefefefefefefefefefefefefefefefefe";
        fs::path orphanDir = makeFakeBuildDir(buildDir, orphanHash);

        // Stale lock: PID 99 is extremely unlikely to be alive in the test
        // environment (mirrors the convention used in buildlock_test.cpp).
        fs::path lockFile = orphanDir / "cog.building";
        writeLockFile(lockFile, 99);

        auto removed = ddb::cleanupBuild(db.get(), "");

        ASSERT_EQ(removed.size(), 1u);
        EXPECT_FALSE(fs::exists(orphanDir));
    }

    TEST(cleanupBuild, IgnoresUnrelatedTopLevelFiles)
    {
        TestArea ta(TEST_NAME, true);
        std::unique_ptr<Database> db;
        std::vector<std::string> hashes;
        setupIndexedDb(ta, db, hashes);

        fs::path buildDir = db->buildDirectory();
        fs::create_directories(buildDir);

        // A regular file with no .pending extension at the top level: must be ignored.
        fs::path stranger = buildDir / "readme.txt";
        std::ofstream(stranger) << "not a build artifact";

        auto removed = ddb::cleanupBuild(db.get(), "");
        EXPECT_TRUE(removed.empty());
        EXPECT_TRUE(fs::exists(stranger));
    }

    TEST(cleanupBuild, CustomOutputPath)
    {
        TestArea ta(TEST_NAME, true);
        std::unique_ptr<Database> db;
        std::vector<std::string> hashes;
        setupIndexedDb(ta, db, hashes);

        // Use a custom output path outside .ddb/build/
        fs::path customDir = ta.getPath("custom_build");
        fs::create_directories(customDir);

        fs::path valid = makeFakeBuildDir(customDir, hashes[0]);
        fs::path orphan = makeFakeBuildDir(
            customDir, "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");

        auto removed = ddb::cleanupBuild(db.get(), customDir.string());

        ASSERT_EQ(removed.size(), 1u);
        EXPECT_TRUE(fs::exists(valid));
        EXPECT_FALSE(fs::exists(orphan));
    }

    TEST(cleanupBuild, RepeatedCallIsIdempotent)
    {
        TestArea ta(TEST_NAME, true);
        std::unique_ptr<Database> db;
        std::vector<std::string> hashes;
        setupIndexedDb(ta, db, hashes);

        fs::path buildDir = db->buildDirectory();
        fs::create_directories(buildDir);
        makeFakeBuildDir(buildDir, hashes[0]);
        makeFakeBuildDir(
            buildDir, "9999999999999999999999999999999999999999999999999999999999999999");

        auto first = ddb::cleanupBuild(db.get(), "");
        EXPECT_EQ(first.size(), 1u);

        auto second = ddb::cleanupBuild(db.get(), "");
        EXPECT_TRUE(second.empty());
    }

    // ---------- C API ----------

    TEST(DDBCleanup, ReturnsJsonArray)
    {
        TestArea ta(TEST_NAME, true);
        std::unique_ptr<Database> db;
        std::vector<std::string> hashes;
        fs::path testFolder = setupIndexedDb(ta, db, hashes);

        fs::path buildDir = db->buildDirectory();
        fs::create_directories(buildDir);
        makeFakeBuildDir(buildDir, hashes[0]);
        fs::path orphan = makeFakeBuildDir(
            buildDir, "5555555555555555555555555555555555555555555555555555555555555555");

        // Close the DB so the C API can re-open it cleanly
        db.reset();

        char *output = nullptr;
        DDBErr err = DDBCleanup(testFolder.string().c_str(), &output);
        ASSERT_EQ(err, DDBERR_NONE) << DDBGetLastError();
        ASSERT_NE(output, nullptr);

        json j = json::parse(output);
        DDBFree(output);

        ASSERT_TRUE(j.is_array());
        ASSERT_EQ(j.size(), 1u);
        std::string p = j[0].get<std::string>();
        EXPECT_NE(p.find(orphan.filename().string()), std::string::npos);
        EXPECT_FALSE(fs::exists(orphan));
    }

    TEST(DDBCleanup, RejectsNullPath)
    {
        char *output = nullptr;
        DDBErr err = DDBCleanup(nullptr, &output);
        EXPECT_EQ(err, DDBERR_EXCEPTION);
    }

    TEST(DDBCleanup, RejectsEmptyPath)
    {
        char *output = nullptr;
        DDBErr err = DDBCleanup("", &output);
        EXPECT_EQ(err, DDBERR_EXCEPTION);
    }

    TEST(DDBCleanup, RejectsNullOutput)
    {
        TestArea ta(TEST_NAME, true);
        fs::path testFolder = ta.getFolder();
        ddb::initIndex(testFolder.string());

        DDBErr err = DDBCleanup(testFolder.string().c_str(), nullptr);
        EXPECT_EQ(err, DDBERR_EXCEPTION);
    }

} // namespace
