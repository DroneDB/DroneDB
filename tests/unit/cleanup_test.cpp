/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "build.h"
#include "buildlock.h"
#include "dbops.h"
#include "ddb.h"
#include "entry.h"
#include "exceptions.h"
#include "gtest/gtest.h"
#include "json.h"
#include "test.h"
#include "testarea.h"

#include <algorithm>
#include <chrono>
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
    // path and (via out-params) the hashes and absolute paths of the indexed
    // entries.
    fs::path setupIndexedDb(TestArea &ta,
                            std::unique_ptr<Database> &db,
                            std::vector<std::string> &outHashes,
                            std::vector<fs::path> &outFiles)
    {
        fs::path testFolder = ta.getFolder();
        ddb::initIndex(testFolder.string());
        db = ddb::open(testFolder.string(), true);

        fs::path f1 = testFolder / "a.txt";
        fs::path f2 = testFolder / "b.txt";
        std::ofstream(f1) << "alpha-content";
        std::ofstream(f2) << "beta-content";

        ddb::addToIndex(db.get(), {f1.string(), f2.string()});

        outFiles = {f1, f2};

        outHashes.clear();
        auto q = db->query(
            "SELECT hash FROM entries WHERE hash IS NOT NULL AND hash <> '' ORDER BY path");
        while (q->fetch())
            outHashes.push_back(q->getText(0));

        return testFolder;
    }

    fs::path setupIndexedDb(TestArea &ta,
                            std::unique_ptr<Database> &db,
                            std::vector<std::string> &outHashes)
    {
        std::vector<fs::path> files;
        return setupIndexedDb(ta, db, outHashes, files);
    }

    // Defined with the orphan temp folder tests below.
    bool wasRemoved(const CleanupResult &r, const fs::path &p);

    // Count how many non-directory entries exist in the DB.
    long countDataEntries(Database *db)
    {
        auto q = db->query("SELECT COUNT(*) FROM entries WHERE type <> ?");
        q->bind(1, entryTypeValue(EntryType::Directory));
        long n = 0;
        if (q->fetch())
            n = q->getInt64(0);
        return n;
    }

    // ---------- Phase 2 (build orphan) tests ----------

    TEST(cleanupBuild, NoBuildDirectoryReturnsEmpty)
    {
        TestArea ta(TEST_NAME, true);
        fs::path testFolder = ta.getFolder();
        ddb::initIndex(testFolder.string());
        auto db = ddb::open(testFolder.string(), true);

        auto result = ddb::cleanupBuild(db.get(), "");
        EXPECT_TRUE(result.removedEntries.empty());
        EXPECT_TRUE(result.removedBuilds.empty());
    }

    TEST(cleanupBuild, EmptyBuildDirectoryReturnsEmpty)
    {
        TestArea ta(TEST_NAME, true);
        fs::path testFolder = ta.getFolder();
        ddb::initIndex(testFolder.string());
        auto db = ddb::open(testFolder.string(), true);

        fs::path buildDir = db->buildDirectory();
        fs::create_directories(buildDir);

        auto result = ddb::cleanupBuild(db.get(), "");
        EXPECT_TRUE(result.removedBuilds.empty());
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

        fs::path d1 = makeFakeBuildDir(buildDir, hashes[0]);
        fs::path d2 = makeFakeBuildDir(buildDir, hashes[1]);

        auto result = ddb::cleanupBuild(db.get(), "");
        EXPECT_TRUE(result.removedEntries.empty());
        EXPECT_TRUE(result.removedBuilds.empty());
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

        fs::path validDir = makeFakeBuildDir(buildDir, hashes[0]);
        fs::path orphanDir = makeFakeBuildDir(
            buildDir, "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef");

        auto result = ddb::cleanupBuild(db.get(), "");

        EXPECT_TRUE(result.removedEntries.empty());
        ASSERT_EQ(result.removedBuilds.size(), 1u);
        EXPECT_EQ(fs::path(result.removedBuilds[0]).filename(), orphanDir.filename());
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

        fs::path validPending = makeFakePendingFile(buildDir, hashes[0]);
        fs::path orphanPending = makeFakePendingFile(
            buildDir, "0000000000000000000000000000000000000000000000000000000000000000");

        auto result = ddb::cleanupBuild(db.get(), "");
        ASSERT_EQ(result.removedBuilds.size(), 1u);
        EXPECT_EQ(fs::path(result.removedBuilds[0]).filename(), orphanPending.filename());
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

        auto result = ddb::cleanupBuild(db.get(), "");
        EXPECT_EQ(result.removedBuilds.size(), 3u);

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

        // Hold a real BuildLock for the duration of the test. With kernel-managed
        // locking (F_OFD_SETLK / flock / CreateFile-no-share), "active" means
        // exactly: another fd in this or another process currently owns the lock.
        // A bare PID written to disk no longer counts as "active".
        const fs::path outputFolder = orphanDir / "cog";
        ddb::BuildLock activeLock(outputFolder.string());

        auto result = ddb::cleanupBuild(db.get(), "");

        EXPECT_TRUE(result.removedBuilds.empty());
        EXPECT_TRUE(fs::exists(orphanDir)) << "Active-locked orphan should not be removed";
        EXPECT_TRUE(fs::exists(outputFolder.string() + ".building"));
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

        // A "stale" lock file is just a leftover from a crashed process: the
        // file exists on disk but the kernel lock was released when the holder
        // died. We simulate that by writing a fake PID into the file with no
        // open fd attached. BuildLock::isHeld queries the kernel lock to
        // discover that no live process holds it.
        fs::path lockFile = orphanDir / "cog.building";
        writeLockFile(lockFile, 2147483646L);

        auto result = ddb::cleanupBuild(db.get(), "");

        ASSERT_EQ(result.removedBuilds.size(), 1u);
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

        fs::path stranger = buildDir / "readme.txt";
        std::ofstream(stranger) << "not a build artifact";

        auto result = ddb::cleanupBuild(db.get(), "");
        EXPECT_TRUE(result.removedBuilds.empty());
        EXPECT_TRUE(fs::exists(stranger));
    }

    TEST(cleanupBuild, IgnoresNonHashDirectory)
    {
        TestArea ta(TEST_NAME, true);
        std::unique_ptr<Database> db;
        std::vector<std::string> hashes;
        setupIndexedDb(ta, db, hashes);

        fs::path buildDir = db->buildDirectory();
        fs::create_directories(buildDir);

        // Directory whose name is not a 64-hex-char hash must be left alone.
        fs::path legacy = buildDir / "legacy_data";
        fs::create_directories(legacy);
        std::ofstream(legacy / "keep.me") << "important";

        // A real orphan alongside it gets removed normally.
        fs::path orphan = makeFakeBuildDir(
            buildDir, "8888888888888888888888888888888888888888888888888888888888888888");

        auto result = ddb::cleanupBuild(db.get(), "");

        ASSERT_EQ(result.removedBuilds.size(), 1u);
        EXPECT_EQ(fs::path(result.removedBuilds[0]).filename(), orphan.filename());
        EXPECT_TRUE(fs::exists(legacy));
        EXPECT_TRUE(fs::exists(legacy / "keep.me"));
        EXPECT_FALSE(fs::exists(orphan));
    }

    TEST(cleanupBuild, IgnoresNonHashPendingFile)
    {
        TestArea ta(TEST_NAME, true);
        std::unique_ptr<Database> db;
        std::vector<std::string> hashes;
        setupIndexedDb(ta, db, hashes);

        fs::path buildDir = db->buildDirectory();
        fs::create_directories(buildDir);

        // *.pending file whose stem isn't a hash must not be removed.
        fs::path stranger = buildDir / "notes.pending";
        std::ofstream(stranger) << "user notes";

        // Real orphan pending alongside it is removed normally.
        fs::path orphan = makeFakePendingFile(
            buildDir, "4444444444444444444444444444444444444444444444444444444444444444");

        auto result = ddb::cleanupBuild(db.get(), "");

        ASSERT_EQ(result.removedBuilds.size(), 1u);
        EXPECT_EQ(fs::path(result.removedBuilds[0]).filename(), orphan.filename());
        EXPECT_TRUE(fs::exists(stranger));
        EXPECT_FALSE(fs::exists(orphan));
    }

    // "<hash>.pending.tmp-<rand>" is the sibling replacePendingMarker() writes
    // before its atomic rename. A crash in between leaves it behind: it is
    // removed once old (even for a still-indexed hash, it is never valid state),
    // kept while fresh (a live writer may be about to rename it), and never
    // matched for a non-hash prefix.
    TEST(cleanupBuild, RemovesStalePendingMarkerTempFiles)
    {
        TestArea ta(TEST_NAME, true);
        std::unique_ptr<Database> db;
        std::vector<std::string> hashes;
        setupIndexedDb(ta, db, hashes);

        fs::path buildDir = db->buildDirectory();
        fs::create_directories(buildDir);

        const auto writeTemp = [&](const std::string &name) {
            const fs::path p = buildDir / name;
            std::ofstream(p) << "0\nsub/m.mtl\n";
            return p;
        };
        const auto makeOld = [](const fs::path &p) {
            fs::last_write_time(p, fs::file_time_type::clock::now() - std::chrono::hours(3));
        };

        const fs::path staleValid = writeTemp(hashes[0] + ".pending.tmp-0123456789abcdef");
        makeOld(staleValid);
        const fs::path staleOrphan = writeTemp(
            "5555555555555555555555555555555555555555555555555555555555555555.pending.tmp-fedcba9876543210");
        makeOld(staleOrphan);
        const fs::path fresh = writeTemp(hashes[1] + ".pending.tmp-00000000000000ff");
        const fs::path stranger = writeTemp("notes.pending.tmp-0123456789abcdef");
        makeOld(stranger);
        const fs::path noSuffix = writeTemp(hashes[0] + ".pending.tmp-");
        makeOld(noSuffix);
        // A real marker of an indexed hash is untouched.
        const fs::path marker = makeFakePendingFile(buildDir, hashes[0]);

        const auto result = ddb::cleanupBuild(db.get(), "");

        EXPECT_FALSE(fs::exists(staleValid));
        EXPECT_FALSE(fs::exists(staleOrphan));
        EXPECT_TRUE(wasRemoved(result, staleValid));
        EXPECT_TRUE(wasRemoved(result, staleOrphan));
        EXPECT_EQ(result.removedBuilds.size(), 2u);
        EXPECT_TRUE(fs::exists(fresh));
        EXPECT_TRUE(fs::exists(stranger));
        EXPECT_TRUE(fs::exists(noSuffix));
        EXPECT_TRUE(fs::exists(marker));

        // The fresh one is collected once it has aged.
        makeOld(fresh);
        const auto again = ddb::cleanupBuild(db.get(), "");
        EXPECT_FALSE(fs::exists(fresh));
        EXPECT_TRUE(wasRemoved(again, fresh));
        EXPECT_TRUE(fs::exists(marker));
    }

    TEST(cleanupBuild, CustomOutputPath)
    {
        TestArea ta(TEST_NAME, true);
        std::unique_ptr<Database> db;
        std::vector<std::string> hashes;
        setupIndexedDb(ta, db, hashes);

        fs::path customDir = ta.getPath("custom_build");
        fs::create_directories(customDir);

        fs::path valid = makeFakeBuildDir(customDir, hashes[0]);
        fs::path orphan = makeFakeBuildDir(
            customDir, "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");

        auto result = ddb::cleanupBuild(db.get(), customDir.string());

        ASSERT_EQ(result.removedBuilds.size(), 1u);
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
        EXPECT_EQ(first.removedBuilds.size(), 1u);

        auto second = ddb::cleanupBuild(db.get(), "");
        EXPECT_TRUE(second.removedBuilds.empty());
        EXPECT_TRUE(second.removedEntries.empty());
    }

    // ---------- Phase 1 (DB stale entries) tests ----------

    TEST(cleanupBuild, RemovesStaleDbEntriesAndTheirBuildFolder)
    {
        TestArea ta(TEST_NAME, true);
        std::unique_ptr<Database> db;
        std::vector<std::string> hashes;
        std::vector<fs::path> files;
        setupIndexedDb(ta, db, hashes, files);
        ASSERT_EQ(files.size(), 2u);
        ASSERT_EQ(hashes.size(), 2u);

        // Pre-populate fake build folders for both entries.
        fs::path buildDir = db->buildDirectory();
        fs::create_directories(buildDir);
        fs::path build0 = makeFakeBuildDir(buildDir, hashes[0]);
        fs::path build1 = makeFakeBuildDir(buildDir, hashes[1]);

        // Delete the source file for entry 0 from the filesystem.
        ASSERT_TRUE(fs::remove(files[0]));

        ASSERT_EQ(countDataEntries(db.get()), 2);

        auto result = ddb::cleanupBuild(db.get(), "");

        // Entry 0 should be removed from the DB.
        ASSERT_EQ(result.removedEntries.size(), 1u);
        EXPECT_EQ(result.removedEntries[0], files[0].filename().string());
        EXPECT_EQ(countDataEntries(db.get()), 1);

        // The build folder of the removed entry is cleaned up by removeFromIndex
        // (phase 1), so phase 2's list should NOT contain it.
        EXPECT_FALSE(fs::exists(build0));
        EXPECT_TRUE(fs::exists(build1));
        EXPECT_TRUE(result.removedBuilds.empty());
    }

    TEST(cleanupBuild, KeepsDbEntriesWhenFilesExist)
    {
        TestArea ta(TEST_NAME, true);
        std::unique_ptr<Database> db;
        std::vector<std::string> hashes;
        std::vector<fs::path> files;
        setupIndexedDb(ta, db, hashes, files);

        const long beforeCount = countDataEntries(db.get());

        auto result = ddb::cleanupBuild(db.get(), "");

        EXPECT_TRUE(result.removedEntries.empty());
        EXPECT_EQ(countDataEntries(db.get()), beforeCount);
    }

    TEST(cleanupBuild, CombinedPhase1AndPhase2)
    {
        TestArea ta(TEST_NAME, true);
        std::unique_ptr<Database> db;
        std::vector<std::string> hashes;
        std::vector<fs::path> files;
        setupIndexedDb(ta, db, hashes, files);

        fs::path buildDir = db->buildDirectory();
        fs::create_directories(buildDir);
        // Build folder for entry 0 (will be removed via phase 1)
        fs::path build0 = makeFakeBuildDir(buildDir, hashes[0]);
        // Standalone orphan with no DB entry (phase 2)
        fs::path orphan = makeFakeBuildDir(
            buildDir, "7777777777777777777777777777777777777777777777777777777777777777");

        // Delete entry 0's source file.
        ASSERT_TRUE(fs::remove(files[0]));

        auto result = ddb::cleanupBuild(db.get(), "");

        EXPECT_EQ(result.removedEntries.size(), 1u);
        ASSERT_EQ(result.removedBuilds.size(), 1u);
        EXPECT_EQ(fs::path(result.removedBuilds[0]).filename(), orphan.filename());
        EXPECT_FALSE(fs::exists(build0));
        EXPECT_FALSE(fs::exists(orphan));
    }

    // ---------- C API ----------

    // ---------- Orphan temp folders inside a still-referenced hash dir ----------

    // Creates <hashDir>/<name>/<file> and returns <hashDir>/<name>.
    fs::path makeSubfolder(const fs::path &hashDir, const std::string &name)
    {
        const fs::path d = hashDir / name;
        fs::create_directories(d);
        std::ofstream(d / "tileset.json") << "{}";
        return d;
    }

    bool wasRemoved(const CleanupResult &r, const fs::path &p)
    {
        return std::find(r.removedBuilds.begin(), r.removedBuilds.end(), p.string()) !=
               r.removedBuilds.end();
    }

    // Generic "<sub>-temp-*" handling is unchanged: removed without a live lock,
    // kept under one.
    TEST(cleanupBuild, GenericTempFolderFollowsItsOwnLock)
    {
        TestArea ta(TEST_NAME, true);
        std::unique_ptr<Database> db;
        std::vector<std::string> hashes;
        setupIndexedDb(ta, db, hashes);
        const fs::path hashDir = makeFakeBuildDir(db->buildDirectory(), hashes[0]);

        const fs::path cogTemp = makeSubfolder(hashDir, "cog-temp-abc123");
        {
            ddb::BuildLock live((hashDir / "cog").string());
            const auto r = ddb::cleanupBuild(db.get(), "");
            EXPECT_TRUE(fs::exists(cogTemp));
            EXPECT_FALSE(wasRemoved(r, cogTemp));
        }

        const auto r = ddb::cleanupBuild(db.get(), "");
        EXPECT_FALSE(fs::exists(cogTemp));
        EXPECT_TRUE(wasRemoved(r, cogTemp));
        EXPECT_TRUE(fs::exists(hashDir / "cog" / "cog.tif"));
    }

    // A Model build holds "bm3d.building" while buildModel3DTiles writes
    // "3dtiles-temp-*" under its own "3dtiles" lock: only that lock protects them.
    TEST(cleanupBuild, TilesTempAndOldKeptUnderLiveTilesLock)
    {
        TestArea ta(TEST_NAME, true);
        std::unique_ptr<Database> db;
        std::vector<std::string> hashes;
        setupIndexedDb(ta, db, hashes);
        const fs::path hashDir = makeFakeBuildDir(db->buildDirectory(), hashes[0]);

        const fs::path tiles = makeSubfolder(hashDir, "3dtiles");
        const fs::path tilesTemp = makeSubfolder(hashDir, "3dtiles-temp-0123456789abcdef");
        const fs::path tilesOld = makeSubfolder(hashDir, "3dtiles-old");

        ddb::BuildLock live(tiles.string());
        const auto r = ddb::cleanupBuild(db.get(), "");
        EXPECT_TRUE(r.removedBuilds.empty());
        EXPECT_TRUE(fs::exists(tilesTemp));
        EXPECT_TRUE(fs::exists(tilesOld));
        EXPECT_TRUE(fs::exists(tiles));
        EXPECT_TRUE(fs::exists(tiles.string() + ".building"));
    }

    // Cleanup follows the build's lock order: primary "bm3d" lock first, then
    // "3dtiles". A live "bm3d" lock (a Model build in progress) therefore protects
    // the tiles siblings too, and cleanup never waits for it.
    TEST(cleanupBuild, TilesTempAndOldKeptUnderLivePrimaryLock)
    {
        TestArea ta(TEST_NAME, true);
        std::unique_ptr<Database> db;
        std::vector<std::string> hashes;
        setupIndexedDb(ta, db, hashes);
        const fs::path hashDir = makeFakeBuildDir(db->buildDirectory(), hashes[0]);

        const fs::path tiles = makeSubfolder(hashDir, "3dtiles");
        const fs::path tilesTemp = makeSubfolder(hashDir, "3dtiles-temp-0123456789abcdef");
        const fs::path tilesOld = makeSubfolder(hashDir, "3dtiles-old");
        ddb::BuildLock bm3dLive((hashDir / "bm3d").string());

        const auto start = std::chrono::steady_clock::now();
        const auto r = ddb::cleanupBuild(db.get(), "");
        const auto elapsed = std::chrono::steady_clock::now() - start;
        EXPECT_LT(elapsed, std::chrono::seconds(5)) << "cleanup must not block on a held lock";

        EXPECT_TRUE(r.removedBuilds.empty());
        EXPECT_TRUE(fs::exists(tilesTemp));
        EXPECT_TRUE(fs::exists(tilesOld));
        EXPECT_TRUE(fs::exists(tiles / "tileset.json"));
        // Cleanup never reached the tiles lock, and still holds nothing.
        EXPECT_FALSE(fs::exists(tiles.string() + ".building"));
        EXPECT_NO_THROW({ ddb::BuildLock probe(tiles.string(), false); });
    }

    // With no lock held at all the siblings are removed and no lock file of either
    // kind is left behind.
    TEST(cleanupBuild, TilesTempAndOldRemovedWithoutAnyLock)
    {
        TestArea ta(TEST_NAME, true);
        std::unique_ptr<Database> db;
        std::vector<std::string> hashes;
        setupIndexedDb(ta, db, hashes);
        const fs::path hashDir = makeFakeBuildDir(db->buildDirectory(), hashes[0]);

        const fs::path tiles = makeSubfolder(hashDir, "3dtiles");
        const fs::path tilesTemp = makeSubfolder(hashDir, "3dtiles-temp-0123456789abcdef");
        const fs::path tilesOld = makeSubfolder(hashDir, "3dtiles-old");

        const auto r = ddb::cleanupBuild(db.get(), "");
        EXPECT_FALSE(fs::exists(tilesTemp));
        EXPECT_FALSE(fs::exists(tilesOld));
        EXPECT_TRUE(wasRemoved(r, tilesTemp));
        EXPECT_TRUE(wasRemoved(r, tilesOld));
        EXPECT_TRUE(fs::exists(tiles / "tileset.json"));
        // Both locks taken by cleanup are released, in order.
        EXPECT_FALSE(fs::exists(tiles.string() + ".building"));
        EXPECT_FALSE(fs::exists((hashDir / "bm3d").string() + ".building"));
        EXPECT_NO_THROW({ ddb::BuildLock probeBm3d((hashDir / "bm3d").string(), false); });
        EXPECT_NO_THROW({ ddb::BuildLock probeTiles(tiles.string(), false); });
    }

    // Trash folders of an earlier cleanup need no lock: they are deleted even
    // while a Model build holds the primary lock, and the live build's temp
    // folder is left alone.
    TEST(cleanupBuild, TilesTrashDeletedUnderLivePrimaryLock)
    {
        TestArea ta(TEST_NAME, true);
        std::unique_ptr<Database> db;
        std::vector<std::string> hashes;
        setupIndexedDb(ta, db, hashes);
        const fs::path hashDir = makeFakeBuildDir(db->buildDirectory(), hashes[0]);

        makeSubfolder(hashDir, "3dtiles");
        const fs::path trash = makeSubfolder(hashDir, "3dtiles-temp-trash-0123456789abcdef");
        const fs::path tilesTemp = makeSubfolder(hashDir, "3dtiles-temp-0123456789abcdef");
        ddb::BuildLock bm3dLive((hashDir / "bm3d").string());

        const auto r = ddb::cleanupBuild(db.get(), "");
        EXPECT_FALSE(fs::exists(trash));
        EXPECT_TRUE(wasRemoved(r, trash));
        EXPECT_TRUE(fs::exists(tilesTemp));
        EXPECT_FALSE(wasRemoved(r, tilesTemp));
    }

    TEST(cleanupBuild, TilesTempAndOldRemovedWithStaleTilesLock)
    {
        TestArea ta(TEST_NAME, true);
        std::unique_ptr<Database> db;
        std::vector<std::string> hashes;
        setupIndexedDb(ta, db, hashes);
        const fs::path hashDir = makeFakeBuildDir(db->buildDirectory(), hashes[0]);

        const fs::path tiles = makeSubfolder(hashDir, "3dtiles");
        const fs::path tilesTemp = makeSubfolder(hashDir, "3dtiles-temp-0123456789abcdef");
        const fs::path tilesOld = makeSubfolder(hashDir, "3dtiles-old");
        // Leftover of a crashed holder: the file exists, no process holds the lock.
        writeLockFile(hashDir / "3dtiles.building", 2147483646L);

        const auto r = ddb::cleanupBuild(db.get(), "");
        EXPECT_FALSE(fs::exists(tilesTemp));
        EXPECT_FALSE(fs::exists(tilesOld));
        EXPECT_TRUE(wasRemoved(r, tilesTemp));
        EXPECT_TRUE(wasRemoved(r, tilesOld));
        EXPECT_TRUE(fs::exists(tiles / "tileset.json"));
    }

    // "3dtiles-old" without "3dtiles/" is the only copy of the previous tileset
    // (an interrupted swap that the next tiles build restores): keep it.
    TEST(cleanupBuild, TilesOldKeptWhileTilesMissing)
    {
        TestArea ta(TEST_NAME, true);
        std::unique_ptr<Database> db;
        std::vector<std::string> hashes;
        setupIndexedDb(ta, db, hashes);
        const fs::path hashDir = makeFakeBuildDir(db->buildDirectory(), hashes[0]);

        const fs::path tilesTemp = makeSubfolder(hashDir, "3dtiles-temp-0123456789abcdef");
        const fs::path tilesOld = makeSubfolder(hashDir, "3dtiles-old");

        const auto r = ddb::cleanupBuild(db.get(), "");
        EXPECT_TRUE(fs::exists(tilesOld / "tileset.json"));
        EXPECT_FALSE(wasRemoved(r, tilesOld));
        EXPECT_FALSE(fs::exists(tilesTemp));
        EXPECT_TRUE(wasRemoved(r, tilesTemp));
    }

    TEST(cleanupBuild, TilesOldKeptWhileFinalSentinelIncomplete)
    {
        for (const std::string state : {"missing", "empty", "directory"})
        {
            SCOPED_TRACE(state);
            TestArea ta(std::string(TEST_NAME) + "-" + state, true);
            std::unique_ptr<Database> db;
            std::vector<std::string> hashes;
            setupIndexedDb(ta, db, hashes);
            const fs::path hashDir = makeFakeBuildDir(db->buildDirectory(), hashes[0]);
            const fs::path tiles = makeSubfolder(hashDir, "3dtiles");
            const fs::path old = makeSubfolder(hashDir, "3dtiles-old");
            fs::remove(tiles / "tileset.json");
            if (state == "empty")
                std::ofstream(tiles / "tileset.json");
            else if (state == "directory")
                fs::create_directory(tiles / "tileset.json");
            const auto result = cleanupBuild(db.get(), "");
            EXPECT_TRUE(fs::exists(old / "tileset.json"));
            EXPECT_FALSE(wasRemoved(result, old));
        }
    }

    std::vector<fs::path> listTilesTrash(const fs::path &hashDir)
    {
        std::vector<fs::path> out;
        for (const auto &e : fs::directory_iterator(hashDir))
            if (e.is_directory() && e.path().filename().string().rfind("3dtiles-temp-trash-", 0) == 0)
                out.push_back(e.path());
        return out;
    }

    // Race: the "3dtiles" lock is held only while the leftovers are renamed
    // aside ("3dtiles-temp-trash-<rand>"); the deletion happens after its release.
    // Trash left by an earlier cleanup is deleted even under a live tiles lock.
    TEST(cleanupBuild, TilesLeftoversDeletedOutsideTheTilesLock)
    {
        TestArea ta(TEST_NAME, true);
        std::unique_ptr<Database> db;
        std::vector<std::string> hashes;
        setupIndexedDb(ta, db, hashes);
        const fs::path hashDir = makeFakeBuildDir(db->buildDirectory(), hashes[0]);
        const fs::path tiles = makeSubfolder(hashDir, "3dtiles");

        {
            const fs::path trash = makeSubfolder(hashDir, "3dtiles-temp-trash-0123456789abcdef");
            const fs::path tilesTemp = makeSubfolder(hashDir, "3dtiles-temp-0123456789abcdef");
            ddb::BuildLock live(tiles.string());
            const auto r = ddb::cleanupBuild(db.get(), "");
            EXPECT_FALSE(fs::exists(trash));
            EXPECT_TRUE(wasRemoved(r, trash));
            EXPECT_TRUE(fs::exists(tilesTemp)); // protected by the live lock
            EXPECT_FALSE(wasRemoved(r, tilesTemp));
        }

#ifndef _WIN32
        if (::geteuid() == 0)
            GTEST_SKIP() << "permission-based delete failure cannot be simulated as root";

        // A leftover whose deletion fails: it is renamed aside under the lock, the
        // lock is released, and the next cleanup deletes the trash folder.
        const fs::path tilesTemp = hashDir / "3dtiles-temp-0123456789abcdef";
        const fs::path stuck = tilesTemp / "stuck";
        fs::create_directories(stuck);
        std::ofstream(stuck / "f.bin") << "x";
        fs::permissions(stuck, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);

        const auto r1 = ddb::cleanupBuild(db.get(), "");
        EXPECT_FALSE(fs::exists(tilesTemp));
        EXPECT_FALSE(wasRemoved(r1, tilesTemp));
        const auto trash = listTilesTrash(hashDir);
        ASSERT_EQ(trash.size(), 1u);
        EXPECT_TRUE(fs::exists(trash[0] / "stuck" / "f.bin"));
        // The lock is not held after the cleanup: a tiles build can start.
        EXPECT_FALSE(fs::exists(tiles.string() + ".building"));
        EXPECT_NO_THROW({ ddb::BuildLock probe(tiles.string(), false); });
        EXPECT_TRUE(fs::exists(tiles / "tileset.json"));

        fs::permissions(trash[0] / "stuck", fs::perms::owner_all, fs::perm_options::replace);
        const auto r2 = ddb::cleanupBuild(db.get(), "");
        EXPECT_FALSE(fs::exists(trash[0]));
        EXPECT_TRUE(wasRemoved(r2, trash[0]));
        EXPECT_TRUE(listTilesTrash(hashDir).empty());
#endif
    }

    TEST(DDBCleanup, ReturnsJsonObjectWithBothLists)
    {
        TestArea ta(TEST_NAME, true);
        std::unique_ptr<Database> db;
        std::vector<std::string> hashes;
        std::vector<fs::path> files;
        fs::path testFolder = setupIndexedDb(ta, db, hashes, files);

        fs::path buildDir = db->buildDirectory();
        fs::create_directories(buildDir);
        fs::path build0 = makeFakeBuildDir(buildDir, hashes[0]);
        fs::path orphan = makeFakeBuildDir(
            buildDir, "5555555555555555555555555555555555555555555555555555555555555555");

        // Delete entry 0's file so phase 1 has work too.
        ASSERT_TRUE(fs::remove(files[0]));

        // Close DB so the C API can re-open cleanly.
        db.reset();

        char *output = nullptr;
        DDBErr err = DDBCleanup(testFolder.string().c_str(), &output);
        ASSERT_EQ(err, DDBERR_NONE) << DDBGetLastError();
        ASSERT_NE(output, nullptr);

        json j = json::parse(output);
        DDBFree(output);

        ASSERT_TRUE(j.is_object());
        ASSERT_TRUE(j.contains("entries"));
        ASSERT_TRUE(j.contains("builds"));
        ASSERT_TRUE(j["entries"].is_array());
        ASSERT_TRUE(j["builds"].is_array());

        ASSERT_EQ(j["entries"].size(), 1u);
        EXPECT_EQ(j["entries"][0].get<std::string>(), files[0].filename().string());

        ASSERT_EQ(j["builds"].size(), 1u);
        std::string p = j["builds"][0].get<std::string>();
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
