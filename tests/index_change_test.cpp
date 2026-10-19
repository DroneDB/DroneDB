/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Change detection: the index stores modification times with sub-second precision, so
// `ddb add` sees an edit made in the same second as the previous add (same size); rows
// written with whole seconds (older versions) are still compared to the second, so nothing
// is re-hashed.

#include <fstream>
#include <thread>

#include "gtest/gtest.h"

#include "dbops.h"
#include "ddb.h"
#include "mio.h"
#include "status.h"
#include "test.h"
#include "testarea.h"

namespace
{
    using namespace ddb;

    void writeFile(const fs::path &p, const std::string &content)
    {
        fs::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary);
        f << content;
    }

    struct Fixture
    {
        fs::path root;
        std::unique_ptr<Database> db;

        explicit Fixture(const std::string &name)
        {
            TestArea ta(name, true);
            root = ta.getFolder("ds");
            initIndex(root.string());
            db = ddb::open(root.string(), false);
        }

        std::string hashOf(const std::string &rel) const
        {
            Entry e;
            return getEntry(db.get(), rel, e) ? e.hash : "<missing>";
        }

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
        const auto before = f.hashOf("a.txt");

        // Same size, right away: the whole-second mtime is almost always the same
        writeFile(f.root / "a.txt", "hello3");
        EXPECT_EQ(f.add("a.txt"), std::vector<std::string>{"a.txt"});

        EXPECT_NE(f.hashOf("a.txt"), before);
        EXPECT_EQ(f.hashOf("a.txt"), Hash::fileSHA256((f.root / "a.txt").string()));
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
        ASSERT_TRUE(getEntry(f.db.get(), "a.txt", e));
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
        EXPECT_EQ(checkUpdate(e, f.root / "a.txt", static_cast<double>(seconds), f.hashOf("a.txt")), NotModified);
        EXPECT_EQ(checkUpdate(e, f.root / "a.txt", static_cast<double>(seconds - 1), f.hashOf("a.txt")), NotModified);
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

} // namespace
