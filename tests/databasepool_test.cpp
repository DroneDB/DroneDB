/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DDB-02 Phase 2 — ddb::open() reuses idle index connections (DatabasePool).

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <thread>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

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

    // What the database file alone holds (immutable=1 ignores the WAL)
    static int countInFileAlone(const fs::path &root, const std::string &sql)
    {
        const auto file = (root / DDB_FOLDER / DDB_DATABASE_FILE).generic_string();
#ifdef _WIN32
        const auto uri = "file:///" + file + "?immutable=1";
#else
        const auto uri = "file:" + file + "?immutable=1";
#endif
        sqlite3 *raw = nullptr;
        EXPECT_EQ(sqlite3_open_v2(uri.c_str(), &raw, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr), SQLITE_OK);
        sqlite3_stmt *stmt = nullptr;
        EXPECT_EQ(sqlite3_prepare_v2(raw, sql.c_str(), -1, &stmt, nullptr), SQLITE_OK);
        int n = -1;
        if (sqlite3_step(stmt) == SQLITE_ROW)
            n = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        sqlite3_close_v2(raw);
        return n;
    }

    static fs::path walOf(const fs::path &root)
    {
        return fs::path((root / DDB_FOLDER / DDB_DATABASE_FILE).string() + "-wal");
    }

    TEST_F(DDB02_Pool, releasedWriterDoesNotCheckpoint)
    {
        // A checkpoint per write costs two or three fsyncs: the frames stay in the WAL
        const auto root = makeIndex(TEST_NAME);
        addFile(root, "b.txt", "bravo");

        EXPECT_EQ(stats().idle, 1u) << "the writer's connection is kept";
        ASSERT_TRUE(fs::exists(walOf(root)));
        EXPECT_GT(fs::file_size(walOf(root)), 0u) << "the release checkpointed";
        EXPECT_TRUE(has(root, "b.txt")) << "a pooled reader sees the write";
    }

    TEST_F(DDB02_Pool, checkpointPutsTheWritesInTheDatabaseFile)
    {
        const auto root = makeIndex(TEST_NAME);
        addFile(root, "b.txt", "bravo");
        ASSERT_TRUE(has(root, "a.txt")); // an idle connection stays open, as in a server
        {
            // total_changes does not count DDL: a schema-only write too
            auto db = ddb::open(root.string(), false);
            db->exec("CREATE TABLE pool_checkpoint_test (x INTEGER)");
        }
        EXPECT_EQ(countInFileAlone(root, "SELECT count(*) FROM entries WHERE path = 'b.txt'"), 0)
            << "expected only in the WAL before the checkpoint";

        EXPECT_TRUE(checkpointIndex(root.string()));

        EXPECT_EQ(countInFileAlone(root, "SELECT count(*) FROM entries WHERE path = 'b.txt'"), 1);
        EXPECT_EQ(countInFileAlone(root, "SELECT count(*) FROM sqlite_master WHERE name = 'pool_checkpoint_test'"), 1);
        EXPECT_EQ(fs::file_size(walOf(root)), 0u);
    }

    TEST_F(DDB02_Pool, checkpointThroughTheCApi)
    {
        const auto root = makeIndex(TEST_NAME);
        addFile(root, "b.txt", "bravo");

        int complete = -1;
        ASSERT_EQ(DDBCheckpoint(root.string().c_str(), &complete), DDBERR_NONE);
        EXPECT_EQ(complete, 1);
        EXPECT_EQ(countInFileAlone(root, "SELECT count(*) FROM entries WHERE path = 'b.txt'"), 1);
        EXPECT_NE(DDBCheckpoint("", nullptr), DDBERR_NONE);
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
        ASSERT_TRUE(checkpointIndex(root.string())); // the file alone holds a.txt
        fs::copy_file(dbFile, snapshot);
        addFile(root, "b.txt", "bravo");
        ASSERT_TRUE(has(root, "b.txt"));

        // Replacing the file needs its connections closed first (the documented contract):
        // the last close moves b.txt out of the WAL into the old file, so the WAL left next
        // to the new file is empty
        DatabasePool::instance().releaseConnections(root);
        fs::rename(snapshot, dbFile); // a new file (inode) at the same path

        EXPECT_FALSE(has(root, "b.txt"));
        EXPECT_TRUE(has(root, "a.txt"));
    }

    TEST_F(DDB02_Pool, connectionToAReplacedFileIsNotReused)
    {
        const auto root = makeIndex(TEST_NAME);
        const auto dbFile = root / DDB_FOLDER / DDB_DATABASE_FILE;
        const auto copy = fs::path(dbFile.string() + ".copy");
        ASSERT_TRUE(checkpointIndex(root.string()));
        fs::copy_file(dbFile, copy);
        ASSERT_TRUE(has(root, "a.txt")); // an idle connection to the current file
        const auto discarded = stats().discarded;

        fs::rename(copy, dbFile); // same content, new inode

        EXPECT_TRUE(has(root, "a.txt"));
        EXPECT_GE(stats().discarded - discarded, 1u) << "the idle connection to the old file was reused";
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
#ifndef _WIN32
    // The pool never closes its connections (they must outlive static destruction), and an
    // idle connection keeps the index's committed changes in dbase.sqlite-wal. A host that
    // exited left them there, and a later process that put another database file in place
    // (a restore, or a test copying a fresh index) had them applied to it: "database disk
    // image is malformed". At a normal exit the pool now moves the WAL into the file.
    TEST_F(DDB02_Pool, exitLeavesTheDataInTheDatabaseFile)
    {
        const auto root = makeIndex(TEST_NAME);
        const auto wal = root / ".ddb" / "dbase.sqlite-wal";

        const pid_t child = fork();
        ASSERT_NE(child, -1);
        if (child == 0)
        {
            addFile(root, "b.txt", "bravo"); // the connection goes back to the pool idle
            std::exit(0);                     // atexit handlers run, the OS closes the handle
        }
        int status = 0;
        waitpid(child, &status, 0);
        ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

        EXPECT_TRUE(!fs::exists(wal) || fs::file_size(wal) == 0) << "WAL frames left after the host exited";

        // The database file alone holds both entries
        TestArea copyArea(std::string(TEST_NAME) + "-copy", true);
        const auto copy = copyArea.getFolder("ds");
        fs::create_directories(copy / ".ddb");
        fs::copy_file(root / ".ddb" / "dbase.sqlite", copy / ".ddb" / "dbase.sqlite");
        EXPECT_TRUE(has(copy, "a.txt"));
        EXPECT_TRUE(has(copy, "b.txt"));
    }
#endif
} // namespace
