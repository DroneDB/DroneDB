/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DDB-02 Phase 2 — ddb::open() reuses idle index connections (DatabasePool).

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <thread>

#include "gtest/gtest.h"

#include "database.h"
#include "databasepool.h"
#include "dbops.h"
#include "ddb.h"
#include "test.h"
#include "testarea.h"

namespace
{
    using namespace ddb;

    // Every test starts with an empty pool and known limits, and restores the defaults
    class DDB02_Pool : public ::testing::Test
    {
    protected:
        DatabasePool::Options saved;

        void SetUp() override
        {
            saved = DatabasePool::instance().options();
            DatabasePool::instance().clear();
            DatabasePool::instance().configure({4, 64, std::chrono::seconds(60)});
        }

        void TearDown() override
        {
            DatabasePool::instance().clear();
            DatabasePool::instance().configure(saved);
        }

        static fs::path makeIndex(const std::string &name, const std::string &folder = "ds")
        {
            TestArea ta(name, folder == "ds");
            const auto root = ta.getFolder(folder);
            initIndex(root.string());
            addFile(root, "a.txt", "alpha");
            return root;
        }

        static void addFile(const fs::path &root, const std::string &name, const std::string &content)
        {
            std::ofstream(root / name) << content;
            auto db = ddb::open(root.string(), false);
            addToIndex(db.get(), {(root / name).string()});
        }

        static bool has(const fs::path &root, const std::string &path)
        {
            auto db = ddb::open(root.string(), false);
            Entry e;
            return getEntry(db.get(), path, e);
        }

        static DatabasePool::Stats stats() { return DatabasePool::instance().stats(); }
    };

    TEST_F(DDB02_Pool, closedConnectionIsReused)
    {
        const auto root = makeIndex(TEST_NAME);
        const auto before = stats();

        for (int i = 0; i < 5; i++)
            EXPECT_TRUE(has(root, "a.txt"));

        const auto after = stats();
        EXPECT_EQ(after.opened - before.opened, 0u);
        EXPECT_EQ(after.reused - before.reused, 5u);
        EXPECT_EQ(after.idle, 1u);
    }

    TEST_F(DDB02_Pool, disabledPoolOpensEveryTime)
    {
        DatabasePool::instance().configure({0, 64, std::chrono::seconds(60)});
        const auto root = makeIndex(TEST_NAME);
        const auto before = stats();

        for (int i = 0; i < 3; i++)
            EXPECT_TRUE(has(root, "a.txt"));

        EXPECT_EQ(stats().reused, before.reused);
        EXPECT_EQ(stats().idle, 0u);
    }

    TEST_F(DDB02_Pool, writesAreVisibleToIdleConnections)
    {
        const auto root = makeIndex(TEST_NAME);
        EXPECT_FALSE(has(root, "b.txt")); // leaves an idle connection with a warm cache

        {
            // Two connections at once: the write goes through the second one
            auto reader = ddb::open(root.string(), false);
            Entry e;
            EXPECT_FALSE(getEntry(reader.get(), "b.txt", e));
            std::ofstream(root / "b.txt") << "bravo";
            auto writer = ddb::open(root.string(), false);
            addToIndex(writer.get(), {(root / "b.txt").string()});
            EXPECT_TRUE(getEntry(reader.get(), "b.txt", e));
        }

        EXPECT_TRUE(has(root, "b.txt"));
    }

    TEST_F(DDB02_Pool, releasedWriterLeavesAnEmptyWal)
    {
        const auto root = makeIndex(TEST_NAME);
        addFile(root, "b.txt", "bravo");

        const auto wal = fs::path((root / DDB_FOLDER / DDB_DATABASE_FILE).string() + "-wal");
        ASSERT_TRUE(fs::exists(wal));
        EXPECT_EQ(fs::file_size(wal), 0u);
        EXPECT_EQ(stats().idle, 1u) << "the writer's connection is kept";
    }

    TEST_F(DDB02_Pool, schemaOnlyWriteReachesTheDatabaseFile)
    {
        // total_changes does not count DDL: the release checkpoint must not rely on it
        const auto root = makeIndex(TEST_NAME);
        ASSERT_TRUE(has(root, "a.txt")); // leaves an idle connection, reused below
        {
            auto db = ddb::open(root.string(), false);
            db->exec("CREATE TABLE pool_checkpoint_test (x INTEGER)");
        }
        EXPECT_EQ(stats().idle, 1u);

        // immutable=1 reads the database file alone, without the WAL
        const auto file = (root / DDB_FOLDER / DDB_DATABASE_FILE).generic_string();
#ifdef _WIN32
        const auto uri = "file:///" + file + "?immutable=1";
#else
        const auto uri = "file:" + file + "?immutable=1";
#endif
        sqlite3 *raw = nullptr;
        ASSERT_EQ(sqlite3_open_v2(uri.c_str(), &raw, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr), SQLITE_OK);
        sqlite3_stmt *stmt = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(raw, "SELECT count(*) FROM sqlite_master WHERE name = 'pool_checkpoint_test'",
                                     -1, &stmt, nullptr),
                  SQLITE_OK);
        ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
        EXPECT_EQ(sqlite3_column_int(stmt, 0), 1) << "the table is only in the WAL";
        sqlite3_finalize(stmt);
        sqlite3_close_v2(raw);
    }

    // Windows cannot delete or replace a file a connection keeps open: there
    // releaseConnections() must come first, and fileIdentity() is a constant
#ifndef _WIN32
    TEST_F(DDB02_Pool, recreatedDatasetIsNotServedByTheOldConnection)
    {
        const auto root = makeIndex(TEST_NAME);
        addFile(root, "b.txt", "bravo");
        ASSERT_TRUE(has(root, "b.txt"));
        const auto discarded = stats().discarded;

        // Deleted and created again at the same path, without releaseConnections()
        fs::remove_all(root / DDB_FOLDER);
        initIndex(root.string());

        EXPECT_FALSE(has(root, "b.txt"));
        EXPECT_EQ(stats().discarded - discarded, 1u);
    }

    TEST_F(DDB02_Pool, databaseFileReplacedByRenameIsDetected)
    {
        const auto root = makeIndex(TEST_NAME);
        const auto dbFile = root / DDB_FOLDER / DDB_DATABASE_FILE;
        const auto snapshot = fs::path(dbFile.string() + ".snapshot");
        fs::copy_file(dbFile, snapshot);
        addFile(root, "b.txt", "bravo");
        ASSERT_TRUE(has(root, "b.txt"));

        fs::rename(snapshot, dbFile); // a new file (inode) at the same path

        EXPECT_FALSE(has(root, "b.txt"));
        EXPECT_TRUE(has(root, "a.txt"));
    }
#endif

    TEST_F(DDB02_Pool, openTransactionIsNotKept)
    {
        const auto root = makeIndex(TEST_NAME);
        const auto discarded = stats().discarded;
        {
            auto db = ddb::open(root.string(), false);
            db->exec("BEGIN");
        }
        EXPECT_EQ(stats().discarded - discarded, 1u);
        EXPECT_EQ(stats().idle, 0u);
        EXPECT_TRUE(has(root, "a.txt"));
    }

    TEST_F(DDB02_Pool, liveStatementIsNotKept)
    {
        const auto root = makeIndex(TEST_NAME);
        const auto discarded = stats().discarded;
        std::unique_ptr<Statement> leftover;
        {
            auto db = ddb::open(root.string(), false);
            leftover = db->query("SELECT path FROM entries");
            leftover->fetch();
        }
        EXPECT_EQ(stats().discarded - discarded, 1u);
        leftover.reset();
        EXPECT_TRUE(has(root, "a.txt"));
    }

    TEST_F(DDB02_Pool, perIndexAndTotalLimits)
    {
        DatabasePool::instance().configure({2, 3, std::chrono::seconds(60)});
        const auto a = makeIndex(TEST_NAME);
        const auto b = makeIndex(TEST_NAME, "ds2");
        DatabasePool::instance().clear();

        {
            std::vector<std::unique_ptr<Database>> open;
            for (int i = 0; i < 4; i++)
                open.push_back(ddb::open(a.string(), false));
        }
        EXPECT_EQ(stats().idle, 2u) << "per index limit";

        {
            std::vector<std::unique_ptr<Database>> open;
            for (int i = 0; i < 2; i++)
                open.push_back(ddb::open(b.string(), false));
        }
        EXPECT_EQ(stats().idle, 3u) << "total limit";
    }

    TEST_F(DDB02_Pool, idleConnectionsExpire)
    {
        DatabasePool::instance().configure({4, 64, std::chrono::seconds(1)});
        const auto root = makeIndex(TEST_NAME);
        ASSERT_EQ(stats().idle, 1u);

        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        const auto before = stats();
        EXPECT_TRUE(has(root, "a.txt"));

        EXPECT_EQ(stats().reused, before.reused) << "the expired connection was reused";
        EXPECT_EQ(stats().opened - before.opened, 1u);
    }

    TEST_F(DDB02_Pool, releaseConnectionsClosesOnlyThatFolder)
    {
        const auto ds = makeIndex(TEST_NAME);
        const auto sibling = makeIndex(TEST_NAME, "ds2"); // same prefix "ds"
        ASSERT_TRUE(has(ds, "a.txt"));
        ASSERT_TRUE(has(sibling, "a.txt"));
        ASSERT_EQ(stats().idle, 2u);

        EXPECT_EQ(DatabasePool::instance().releaseConnections(ds), 1u);
        EXPECT_EQ(stats().idle, 1u);

        int closed = -1;
        ASSERT_EQ(DDBReleaseConnections(ds.parent_path().string().c_str(), &closed), DDBERR_NONE);
        EXPECT_EQ(closed, 1);
        EXPECT_EQ(stats().idle, 0u);
    }

#ifdef _WIN32
    TEST_F(DDB02_Pool, releaseConnectionsIgnoresLetterCase)
    {
        // Windows paths are case-insensitive: another spelling names the same folder
        const auto ds = makeIndex(TEST_NAME);
        ASSERT_TRUE(has(ds, "a.txt"));
        ASSERT_EQ(stats().idle, 1u);

        auto upper = ds.string();
        std::transform(upper.begin(), upper.end(), upper.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        ASSERT_NE(upper, ds.string());
        EXPECT_EQ(DatabasePool::instance().releaseConnections(upper), 1u);
        EXPECT_EQ(stats().idle, 0u);
    }
#endif

    TEST_F(DDB02_Pool, reopenUsesANewConnection)
    {
        const auto root = makeIndex(TEST_NAME);
        auto db = ddb::open(root.string(), false);
        db->reopen();
        Entry e;
        EXPECT_TRUE(getEntry(db.get(), "a.txt", e));
        db.reset();
        EXPECT_EQ(stats().idle, 1u);
    }

    TEST_F(DDB02_Pool, concurrentCallsShareConnections)
    {
        const auto root = makeIndex(TEST_NAME);
        constexpr int threads = 8;
        constexpr int iterations = 200;
        std::atomic<int> ok{0};
        std::vector<std::string> errors(threads);
        const auto before = stats();

        std::vector<std::thread> pool;
        for (int t = 0; t < threads; t++)
            pool.emplace_back([&, t]
                              {
                for (int i = 0; i < iterations; i++)
                {
                    try
                    {
                        char *out = nullptr;
                        if (DDBGet(root.string().c_str(), "a.txt", &out) == DDBERR_NONE)
                        {
                            ok++;
                            DDBFree(out);
                        }
                        else
                            errors[t] = DDBGetLastError();
                    }
                    catch (const std::exception &ex)
                    {
                        errors[t] = ex.what();
                    }
                } });
        for (auto &th : pool)
            th.join();

        for (const auto &err : errors)
            EXPECT_TRUE(err.empty()) << err;
        EXPECT_EQ(ok.load(), threads * iterations);
        const auto after = stats();
        EXPECT_GT(after.reused - before.reused, static_cast<uint64_t>(threads * iterations / 2));
        EXPECT_LE(after.idle, 4u);
    }
} // namespace
