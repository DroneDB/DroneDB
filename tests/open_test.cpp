/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DDB-02 (Phase 1) — cost of the per-call index open.
//
//  - opens are no longer serialized by a process-wide mutex, and SQLite's allocation
//    statistics (a global mutex on every malloc) are off, so concurrent opens scale;
//  - the -wal/-shm files are kept when the last connection closes (they used to be
//    deleted and recreated by every read-only call) and the WAL is truncated to zero
//    bytes once checkpointed, so no stale frames sit next to the database file;
//  - the journal mode is only written when the database is not in WAL mode yet.

#include <atomic>
#include <fstream>
#include <thread>

#include "gtest/gtest.h"

#include "database.h"
#include "dbops.h"
#include "ddb.h"
#include "test.h"
#include "testarea.h"

namespace
{
    using namespace ddb;

    fs::path makeIndex(const std::string &name)
    {
        TestArea ta(name, true);
        const auto root = ta.getFolder("ds");
        initIndex(root.string());
        {
            std::ofstream f(root / "a.txt");
            f << "alpha";
        }
        auto db = ddb::open(root.string(), false);
        addToIndex(db.get(), {(root / "a.txt").string()});
        return root;
    }

    fs::path dbFile(const fs::path &root) { return root / DDB_FOLDER / DDB_DATABASE_FILE; }
    fs::path walFile(const fs::path &root) { return fs::path(dbFile(root).string() + "-wal"); }

    TEST(DDB02_Open, sqliteMemoryStatisticsAreOff)
    {
        const auto root = makeIndex(TEST_NAME);
        auto db = ddb::open(root.string(), false);
        Entry e;
        ASSERT_TRUE(getEntry(db.get(), "a.txt", e));

        // Tracked only when SQLITE_CONFIG_MEMSTATUS is on (the global allocator mutex)
        EXPECT_EQ(sqlite3_memory_used(), 0);
    }

    TEST(DDB02_Open, walIsKeptAndEmptyAfterLastClose)
    {
        const auto root = makeIndex(TEST_NAME);

        {
            auto db = ddb::open(root.string(), false);
            Entry e;
            ASSERT_TRUE(getEntry(db.get(), "a.txt", e));
        }

        ASSERT_TRUE(fs::exists(walFile(root))) << "the last close deleted the WAL";
        EXPECT_EQ(fs::file_size(walFile(root)), 0u) << "checkpointed frames left in the WAL";
    }

    TEST(DDB02_Open, writesAreInTheMainFileAfterClose)
    {
        const auto root = makeIndex(TEST_NAME);

        {
            auto db = ddb::open(root.string(), false);
            std::ofstream(root / "b.txt") << "bravo";
            addToIndex(db.get(), {(root / "b.txt").string()});
            EXPECT_GT(fs::file_size(walFile(root)), 0u);
        }
        EXPECT_EQ(fs::file_size(walFile(root)), 0u);

        // Copying only dbase.sqlite (templates, backups, the tests' fixtures) keeps the data
        TestArea copyArea(std::string(TEST_NAME) + "-copy", true);
        const auto copy = copyArea.getFolder("ds");
        fs::create_directories(copy / DDB_FOLDER);
        fs::copy_file(dbFile(root), dbFile(copy));

        auto db = ddb::open(copy.string(), false);
        Entry e;
        EXPECT_TRUE(getEntry(db.get(), "b.txt", e));
    }

    TEST(DDB02_Open, replacedDatabaseFileIsNotMixedWithOldWal)
    {
        const auto root = makeIndex(TEST_NAME);
        const auto snapshot = dbFile(root).string() + ".snapshot";
        fs::copy_file(dbFile(root), snapshot);

        {
            auto db = ddb::open(root.string(), false);
            std::ofstream(root / "b.txt") << "bravo";
            addToIndex(db.get(), {(root / "b.txt").string()});
        }

        // Put the older file back next to the kept -wal/-shm
        fs::copy_file(snapshot, dbFile(root), fs::copy_options::overwrite_existing);

        auto db = ddb::open(root.string(), false);
        Entry e;
        EXPECT_TRUE(getEntry(db.get(), "a.txt", e));
        EXPECT_FALSE(getEntry(db.get(), "b.txt", e)) << "frames of the old WAL were replayed";
    }

    TEST(DDB02_Open, readOnlyOpenDoesNotWriteTheDatabaseFile)
    {
        const auto root = makeIndex(TEST_NAME);
        { auto db = ddb::open(root.string(), false); }

        const auto before = fs::last_write_time(dbFile(root));
        const auto sizeBefore = fs::file_size(dbFile(root));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        for (int i = 0; i < 5; i++)
        {
            auto db = ddb::open(root.string(), false);
            Entry e;
            ASSERT_TRUE(getEntry(db.get(), "a.txt", e));
        }

        EXPECT_EQ(fs::last_write_time(dbFile(root)), before);
        EXPECT_EQ(fs::file_size(dbFile(root)), sizeBefore);
    }

    TEST(DDB02_Open, rollbackJournalDatabaseIsSwitchedToWal)
    {
        const auto root = makeIndex(TEST_NAME);
        {
            Database db;
            db.open(dbFile(root).string());
            db.exec("PRAGMA journal_mode=DELETE;");
            EXPECT_EQ(db.getJournalMode(), "delete");
        }

        // A plain connection (no afterOpen) reads the mode stored in the header
        sqlite3 *raw = nullptr;
        ASSERT_EQ(sqlite3_open(dbFile(root).string().c_str(), &raw), SQLITE_OK);
        sqlite3_stmt *stmt = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(raw, "PRAGMA journal_mode", -1, &stmt, nullptr), SQLITE_OK);
        ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
        EXPECT_STREQ(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0)), "delete");
        sqlite3_finalize(stmt);
        sqlite3_close_v2(raw);

        auto db = ddb::open(root.string(), false);
        EXPECT_EQ(db->getJournalMode(), "wal");
    }

    TEST(DDB02_Open, concurrentOpensAndReadsSucceed)
    {
        const auto root = makeIndex(TEST_NAME);
        constexpr int threads = 8;
        constexpr int iterations = 40;
        std::atomic<int> ok{0};
        std::atomic<int> failed{0};
        std::vector<std::string> errors(threads);

        std::vector<std::thread> pool;
        for (int t = 0; t < threads; t++)
        {
            pool.emplace_back([&, t]
                              {
                for (int i = 0; i < iterations; i++)
                {
                    try
                    {
                        auto db = ddb::open(root.string(), false);
                        Entry e;
                        if (getEntryWithMeta(db.get(), "a.txt", e) && e.path == "a.txt")
                            ok++;
                        else
                            failed++;
                    }
                    catch (const std::exception &ex)
                    {
                        failed++;
                        errors[t] = ex.what();
                    }
                } });
        }
        for (auto &th : pool)
            th.join();

        for (const auto &err : errors)
            EXPECT_TRUE(err.empty()) << err;
        EXPECT_EQ(failed.load(), 0);
        EXPECT_EQ(ok.load(), threads * iterations);
    }

    TEST(DDB02_Open, concurrentInitIndexSucceeds)
    {
        TestArea ta(TEST_NAME, true);
        constexpr int threads = 6;
        std::vector<std::thread> pool;
        std::vector<std::string> errors(threads);
        for (int t = 0; t < threads; t++)
        {
            pool.emplace_back([&, t]
                              {
                try
                {
                    const auto dir = ta.getFolder("ds" + std::to_string(t));
                    initIndex(dir.string());
                    auto db = ddb::open(dir.string(), false);
                    if (!db->tableExists("entries"))
                        errors[t] = "no entries table";
                }
                catch (const std::exception &ex)
                {
                    errors[t] = ex.what();
                } });
        }
        for (auto &th : pool)
            th.join();
        for (const auto &err : errors)
            EXPECT_TRUE(err.empty()) << err;
    }
} // namespace
