/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Change detection and index paths:
//
//  - the index stores modification times with sub-second precision, so `ddb add` sees an
//    edit made in the same second as the previous add (same size); rows written with whole
//    seconds (older versions) are still compared to the second, so nothing is re-hashed;
//  - the dataset root is never an entry (`ddb add .` used to index it with an empty path),
//    and such a row is removed when the index is opened;
//  - "/data/ds" is not a parent of "/data/ds2".

#include <fstream>
#include <thread>

#include "gtest/gtest.h"

#include "databasepool.h"
#include "dbops.h"
#include "ddb.h"
#include "exceptions.h"
#include "mio.h"
#include "status.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace
{
    using namespace ddb;

    struct Fixture : IndexFixture
    {
        using IndexFixture::IndexFixture;

        // The stored mtime and its SQLite type
        std::pair<double, std::string> storedMtime(const std::string &rel) const
        {
            auto q = db->query("SELECT mtime, typeof(mtime) FROM entries WHERE path = ?");
            q->bind(1, rel);
            EXPECT_TRUE(q->fetch());
            return {q->getDouble(0), q->getText(1)};
        }

        std::vector<std::string> add(const std::string &rel)
        {
            std::vector<std::string> reported;
            addToIndex(db.get(), {(root / rel).string()}, [&reported](const Entry &e, bool)
                       { reported.push_back(e.path); return true; });
            return reported;
        }
    };

    TEST(IndexChange, editInTheSameSecondIsDetected)
    {
        Fixture f(TEST_NAME);
        writeFile(f.root / "a.txt", "hello2");
        f.add("a.txt");
        const auto before = hashOf(f.db.get(), "a.txt");

        // Same size, right away: the whole-second mtime is almost always the same
        writeFile(f.root / "a.txt", "hello3");
        EXPECT_EQ(f.add("a.txt"), std::vector<std::string>{"a.txt"});

        EXPECT_NE(hashOf(f.db.get(), "a.txt"), before);
        EXPECT_EQ(hashOf(f.db.get(), "a.txt"), Hash::fileSHA256((f.root / "a.txt").string()));
    }

    TEST(IndexChange, storesSubSecondTimeButReportsSeconds)
    {
        Fixture f(TEST_NAME);
        writeFile(f.root / "a.txt", "a");
        const double fileMtime = io::Path(f.root / "a.txt").getModifiedTimePrecise();
        f.add("a.txt");

        const auto [stored, type] = f.storedMtime("a.txt");
        EXPECT_TRUE(io::sameModifiedTime(stored, fileMtime));
        if (fileMtime != std::floor(fileMtime))
            EXPECT_EQ(type, "real");

        // Listings and the C API keep whole seconds
        Entry e;
        ASSERT_TRUE(loadEntry(f.db.get(), "a.txt", e));
        EXPECT_EQ(e.mtime, static_cast<time_t>(std::floor(fileMtime)));

        // Unchanged file: not reported again
        EXPECT_TRUE(f.add("a.txt").empty());
    }

    TEST(IndexChange, wholeSecondRowsAreComparedToTheSecond)
    {
        // A row written by an older version holds whole seconds: an unchanged file must not
        // look modified (it would be re-hashed by every add)
        Fixture f(TEST_NAME);
        writeFile(f.root / "a.txt", "a");
        f.add("a.txt");
        const auto seconds = static_cast<long long>(std::floor(f.storedMtime("a.txt").first));
        auto q = f.db->query("UPDATE entries SET mtime = ? WHERE path = 'a.txt'");
        q->bind(1, seconds);
        q->execute();

        EXPECT_TRUE(f.add("a.txt").empty());
        EXPECT_EQ(f.storedMtime("a.txt").second, "integer");

        Entry e;
        EXPECT_EQ(checkUpdate(e, f.root / "a.txt", static_cast<double>(seconds), hashOf(f.db.get(), "a.txt")), NotModified);
        EXPECT_EQ(checkUpdate(e, f.root / "a.txt", static_cast<double>(seconds - 1), hashOf(f.db.get(), "a.txt")), NotModified);
        EXPECT_EQ(checkUpdate(e, f.root / "a.txt", static_cast<double>(seconds - 1), "other"), Modified);
    }

    TEST(IndexChange, syncLocalMTimesRestoresSubSecondTimes)
    {
        Fixture f(TEST_NAME);
        writeFile(f.root / "a.txt", "a");
        f.add("a.txt");
        const auto stored = f.storedMtime("a.txt").first;

        io::Path(f.root / "a.txt").setModifiedTime(static_cast<time_t>(std::floor(stored)) - 100);
        syncLocalMTimes(f.db.get(), {"a.txt"});

        EXPECT_TRUE(io::sameModifiedTime(stored, io::Path(f.root / "a.txt").getModifiedTimePrecise()));
        EXPECT_TRUE(f.add("a.txt").empty());
    }

    TEST(IndexChange, statusSeesSameSecondEdit)
    {
        Fixture f(TEST_NAME);
        writeFile(f.root / "a.txt", "hello2");
        f.add("a.txt");
        writeFile(f.root / "a.txt", "hello3");

        FileStatus status = NotIndexed;
        statusIndex(f.db.get(), [&status](const FileStatus s, const std::string &file)
                    { if (file == "a.txt") status = s; });
        EXPECT_EQ(status, Modified);
    }

    TEST(IndexChange, rootIsNeverAnEntry)
    {
        Fixture f(TEST_NAME);
        writeFile(f.root / "dir" / "a.txt", "a");

        addToIndex(f.db.get(), {f.root.string()});
        addToIndex(f.db.get(), {(f.root / ".").string()});

        EXPECT_EQ(hashOf(f.db.get(), ""), "<missing>");
        EXPECT_NE(hashOf(f.db.get(), "dir/a.txt"), "<missing>");
        EXPECT_EQ(hashOf(f.db.get(), "dir"), "");
    }

    TEST(IndexChange, rootEntryIsRemovedOnOpen)
    {
        Fixture f(TEST_NAME);
        writeFile(f.root / "a.txt", "a");
        f.add("a.txt");
        f.db->exec("INSERT INTO entries (path, hash, type, properties, mtime, size, depth) "
                   "VALUES ('', '', 7, '{}', 0, 0, 0)");
        ASSERT_EQ(hashOf(f.db.get(), ""), "");
        f.db->close();
        DatabasePool::instance().releaseConnections(f.root);

        f.db = ddb::open(f.root.string(), false);
        EXPECT_EQ(hashOf(f.db.get(), ""), "<missing>");
        EXPECT_NE(hashOf(f.db.get(), "a.txt"), "<missing>");
    }

    TEST(IndexChange, parentNeedsASeparator)
    {
        TestArea ta(TEST_NAME, true);
        const auto base = ta.getFolder("x");
        fs::create_directories(base / "ds");
        fs::create_directories(base / "ds2");

        EXPECT_TRUE(io::Path(base / "ds").isParentOf(base / "ds" / "a.txt"));
        EXPECT_FALSE(io::Path(base / "ds").isParentOf(base / "ds2" / "a.txt"));
        EXPECT_FALSE(io::Path(base / "ds").isParentOf(base / "ds"));
        EXPECT_TRUE(io::Path("/").isParentOf("/tmp"));

        // A path next to the dataset is not inside it
        initIndex((base / "ds").string());
        const auto db = ddb::open((base / "ds").string(), false);
        writeFile(base / "ds2" / "a.txt", "a");
        EXPECT_THROW(addToIndex(db.get(), {(base / "ds2" / "a.txt").string()}), FSException);
    }
} // namespace
