/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DDB-01 — path lookups use the primary key and return each entry once.
//
//  - an exact path is an index seek (`path = ?`), a folder is a key range, only
//    patterns with `*` fall back to LIKE;
//  - metadata is attached per entry, so an entry with N metadata rows is returned once
//    (it used to be returned N times and DDBGet failed with "Multiple entries");
//  - exact lookups are case-sensitive and treat `_` and `%` literally (LIKE matched
//    "IMG_1.JPG" against "imgx1.jpg", so removing one entry could remove another).

#include <fstream>

#include "gtest/gtest.h"

#include "dbops.h"
#include "ddb.h"
#include "exceptions.h"
#include "metamanager.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace
{
    using namespace ddb;

    struct Fixture : IndexFixture
    {
        explicit Fixture(const std::string &name) : IndexFixture(name)
        {
            writeFile(root / "a.txt", "alpha");
            writeFile(root / "dir/b.txt", "bravo");
            writeFile(root / "dir/c.txt", "charlie");
            writeFile(root / "dir2/d.txt", "delta");
            addToIndex(db.get(), {(root / "a.txt").string(), (root / "dir").string(),
                                  (root / "dir2").string()});
        }

        void add(const std::string &rel, const std::string &content)
        {
            writeFile(root / rel, content);
            addToIndex(db.get(), {(root / rel).string()});
        }

        std::vector<std::string> plan(const PathPredicate &p) const
        {
            auto q = db->query("EXPLAIN QUERY PLAN SELECT e.path FROM entries e WHERE " + p.sql);
            for (size_t i = 0; i < p.bindings.size(); i++)
                q->bind(static_cast<int>(i) + 1, p.bindings[i]);
            std::vector<std::string> rows;
            while (q->fetch())
                rows.push_back(q->getText(3));
            return rows;
        }
    };

    bool containsPath(const std::vector<Entry> &entries, const std::string &path)
    {
        return std::any_of(entries.begin(), entries.end(), [&](const Entry &e)
                           { return e.path == path; });
    }

    TEST(DDB01_Lookup, entryWithSeveralMetaRowsIsReturnedOnce)
    {
        Fixture fx(TEST_NAME);
        MetaManager mm(fx.db.get());
        mm.add("annotations", R"({"a":1})", "a.txt", fx.root.string());
        mm.add("annotations", R"({"a":2})", "a.txt", fx.root.string());
        mm.add("annotations", R"({"a":3})", "a.txt", fx.root.string());
        mm.set("note", R"("hello")", "a.txt", fx.root.string());

        const auto entries = getMatchingEntries(fx.db.get(), "a.txt");
        ASSERT_EQ(entries.size(), 1u);
        ASSERT_TRUE(entries[0].meta.contains("annotations"));
        EXPECT_EQ(entries[0].meta["annotations"].size(), 3u);
        ASSERT_TRUE(entries[0].meta.contains("note"));
        EXPECT_EQ(entries[0].meta["note"]["data"], "hello");

        // Folder listings and wildcard searches return the entry once too
        EXPECT_EQ(getMatchingEntries(fx.db.get(), "*").size(), 6u);
    }

    TEST(DDB01_Lookup, entryWithoutMetaHasEmptyMeta)
    {
        Fixture fx(TEST_NAME);
        const auto entries = getMatchingEntries(fx.db.get(), "dir/b.txt");
        ASSERT_EQ(entries.size(), 1u);
        EXPECT_TRUE(entries[0].meta.empty());

        json j;
        entries[0].toJSON(j);
        EXPECT_FALSE(j.contains("meta"));
    }

    TEST(DDB01_Lookup, cApiGetWorksWithSeveralMetaRows)
    {
        Fixture fx(TEST_NAME);
        MetaManager mm(fx.db.get());
        mm.add("annotations", R"({"a":1})", "a.txt", fx.root.string());
        mm.add("annotations", R"({"a":2})", "a.txt", fx.root.string());
        fx.db->close();

        char *out = nullptr;
        const DDBErr rc = DDBGet(fx.root.string().c_str(), "a.txt", &out);
        ASSERT_EQ(rc, DDBERR_NONE) << DDBGetLastError();
        ASSERT_NE(out, nullptr);
        const auto j = json::parse(out);
        DDBFree(out);
        EXPECT_EQ(j["path"], "a.txt");
        EXPECT_EQ(j["meta"]["annotations"].size(), 2u);
    }

    TEST(DDB01_Lookup, cApiGetReturnsFolderEntry)
    {
        Fixture fx(TEST_NAME);
        fx.db->close();

        char *out = nullptr;
        ASSERT_EQ(DDBGet(fx.root.string().c_str(), "dir", &out), DDBERR_NONE) << DDBGetLastError();
        const auto j = json::parse(out);
        DDBFree(out);
        EXPECT_EQ(j["path"], "dir");
        EXPECT_EQ(j["type"].get<int>(), static_cast<int>(EntryType::Directory));
    }

    TEST(DDB01_Lookup, cApiGetMissingEntryIsNotFound)
    {
        Fixture fx(TEST_NAME);
        fx.db->close();

        char *out = nullptr;
        EXPECT_EQ(DDBGet(fx.root.string().c_str(), "missing.txt", &out), DDBERR_NOTFOUND);
        EXPECT_EQ(out, nullptr);
        EXPECT_NE(std::string(DDBGetLastError()).find("missing.txt"), std::string::npos);

        // Not a pattern: "*.txt" matches several entries but no entry has that path
        EXPECT_EQ(DDBGet(fx.root.string().c_str(), "*.txt", &out), DDBERR_NOTFOUND);
    }

    TEST(DDB01_Lookup, folderPrefixDoesNotLeakIntoSiblingFolders)
    {
        Fixture fx(TEST_NAME);
        // "dir" must not match "dir2/..."
        const auto entries = getMatchingEntries(fx.db.get(), "dir", 0, true);
        for (const auto &e : entries)
            EXPECT_EQ(e.path.rfind("dir/", 0), 0u) << "unexpected " << e.path;
        EXPECT_EQ(entries.size(), 2u);
    }

    TEST(DDB01_Lookup, folderDepthLimitStillApplies)
    {
        Fixture fx(TEST_NAME);
        fs::create_directories(fx.root / "dir" / "sub");
        fx.add("dir/sub/e.txt", "echo");

        // depth of dir/x is 1, dir/sub/x is 2: maxRecursionDepth 2 keeps depth <= 1
        const auto shallow = getMatchingEntries(fx.db.get(), "dir", 2, true);
        EXPECT_TRUE(containsPath(shallow, "dir/sub"));
        EXPECT_FALSE(containsPath(shallow, "dir/sub/e.txt"));

        const auto all = getMatchingEntries(fx.db.get(), "dir", 0, true);
        EXPECT_TRUE(containsPath(all, "dir/sub/e.txt"));
    }

    TEST(DDB01_Lookup, wildcardStillSupported)
    {
        Fixture fx(TEST_NAME);
        const auto entries = getMatchingEntries(fx.db.get(), "*.txt");
        EXPECT_EQ(entries.size(), 4u);

        const auto inDir = getMatchingEntries(fx.db.get(), "dir/*");
        EXPECT_EQ(inDir.size(), 2u);
    }

    TEST(DDB01_Lookup, emptyPathMatchesWholeIndex)
    {
        Fixture fx(TEST_NAME);
        EXPECT_EQ(getMatchingEntries(fx.db.get(), "").size(), 6u);
        // Top level only
        EXPECT_EQ(getMatchingEntries(fx.db.get(), "", 1).size(), 3u);
    }

    TEST(DDB01_Lookup, exactLookupIsLiteral)
    {
        Fixture fx(TEST_NAME);
        fx.add("IMG_1.JPG", "one");
        fx.add("imgx1.jpg", "other");
        fx.add("100%.txt", "percent");

        // LIKE treated '_' as any character and ignored case
        const auto img = getMatchingEntries(fx.db.get(), "IMG_1.JPG");
        ASSERT_EQ(img.size(), 1u);
        EXPECT_EQ(img[0].path, "IMG_1.JPG");

        EXPECT_TRUE(getMatchingEntries(fx.db.get(), "A.TXT").empty());
        EXPECT_EQ(getMatchingEntries(fx.db.get(), "100%.txt").size(), 1u);
    }

    TEST(DDB01_Lookup, removeDoesNotRemoveLookalikeEntries)
    {
        Fixture fx(TEST_NAME);
        fx.add("IMG_1.JPG", "one");
        fx.add("imgx1.jpg", "other");

        removeFromIndex(fx.db.get(), {(fx.root / "IMG_1.JPG").string()});

        Entry e;
        EXPECT_FALSE(getEntry(fx.db.get(), "IMG_1.JPG", e));
        EXPECT_TRUE(getEntry(fx.db.get(), "imgx1.jpg", e)) << "removing IMG_1.JPG removed imgx1.jpg";
    }

    TEST(DDB01_Lookup, removeFolderKeepsSiblingFolder)
    {
        Fixture fx(TEST_NAME);
        removeFromIndex(fx.db.get(), {(fx.root / "dir").string()});

        Entry e;
        EXPECT_FALSE(getEntry(fx.db.get(), "dir", e));
        EXPECT_FALSE(getEntry(fx.db.get(), "dir/b.txt", e));
        EXPECT_TRUE(getEntry(fx.db.get(), "dir2", e));
        EXPECT_TRUE(getEntry(fx.db.get(), "dir2/d.txt", e));
    }

    TEST(DDB01_Lookup, deleteFromIndexEmptyQueryDeletesNothing)
    {
        Fixture fx(TEST_NAME);
        EXPECT_EQ(deleteFromIndex(fx.db.get(), "", false), 0);
        EXPECT_EQ(deleteFromIndex(fx.db.get(), "", true), 0);
        EXPECT_EQ(getMatchingEntries(fx.db.get(), "").size(), 6u);
    }

    TEST(DDB01_Lookup, moveFolderKeepsSiblingFolder)
    {
        Fixture fx(TEST_NAME);
        moveEntry(fx.db.get(), "dir", "moved");

        Entry e;
        EXPECT_TRUE(getEntry(fx.db.get(), "moved/b.txt", e));
        EXPECT_TRUE(getEntry(fx.db.get(), "dir2/d.txt", e)) << "moving dir moved dir2";
    }

    TEST(DDB01_Lookup, predicateStrategies)
    {
        const auto exact = buildPathPredicate("a/b_c.txt", false);
        EXPECT_EQ(exact.sql, "path = ?");
        EXPECT_EQ(exact.bindings, std::vector<std::string>{"a/b_c.txt"});

        const auto folder = buildPathPredicate("a/b", true, "e.path");
        EXPECT_EQ(folder.sql, "e.path >= ? AND e.path < ?");
        EXPECT_EQ(folder.bindings, (std::vector<std::string>{"a/b/", "a/b0"}));

        const auto wildcard = buildPathPredicate("a/*.txt", false);
        EXPECT_EQ(wildcard.sql, "path LIKE ? ESCAPE '/'");
        EXPECT_EQ(wildcard.bindings, std::vector<std::string>{"a//%.txt"});

        const auto wildcardFolder = buildPathPredicate("a*", true);
        EXPECT_EQ(wildcardFolder.bindings, std::vector<std::string>{"a%//%"});

        EXPECT_TRUE(buildPathPredicate("", false).sql.empty());
        EXPECT_TRUE(buildPathPredicate("", true).sql.empty());
    }

    TEST(DDB01_Lookup, exactAndFolderPredicatesUseThePrimaryKey)
    {
        Fixture fx(TEST_NAME);

        const auto exactPlan = fx.plan(buildPathPredicate("dir/b.txt", false, "e.path"));
        ASSERT_FALSE(exactPlan.empty());
        EXPECT_NE(exactPlan[0].find("SEARCH"), std::string::npos) << exactPlan[0];
        EXPECT_NE(exactPlan[0].find("(path=?)"), std::string::npos) << exactPlan[0];

        const auto folderPlan = fx.plan(buildPathPredicate("dir", true, "e.path"));
        ASSERT_FALSE(folderPlan.empty());
        EXPECT_NE(folderPlan[0].find("SEARCH"), std::string::npos) << folderPlan[0];
        EXPECT_NE(folderPlan[0].find("path>? AND path<?"), std::string::npos) << folderPlan[0];
    }
} // namespace
