/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DDB-03 — getDelta() is a merge-join over the two sorted stamps (it used to search the
// other list linearly for every entry: 5 s for 50k vs 50k), and DDBGetStamp writes the
// stamp without building a json document.
//
//  - semantics*: the add/remove rules the merge-join must keep;
//  - matchesReferenceImplementation: randomised stamp pairs give the same delta as the
//    previous O(n·m) implementation (kept below as the reference);
//  - isLinear*: timing, skipped with DDB03_SKIP_TIMING=1 (Debug builds).

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <random>

#include "gtest/gtest.h"

#include "dbops.h"
#include "ddb.h"
#include "delta.h"
#include "exceptions.h"
#include "metamanager.h"
#include "test.h"
#include "testarea.h"

namespace
{
    using namespace ddb;

    json makeStamp(const std::vector<std::pair<std::string, std::string>> &entries,
                   const std::vector<std::string> &meta = {})
    {
        json s;
        s["checksum"] = "";
        s["entries"] = json::array();
        for (const auto &[p, h] : entries)
            s["entries"].push_back(json::object({{p, h}}));
        s["meta"] = meta;
        return s;
    }

    template <typename Action>
    std::vector<std::string> paths(const std::vector<Action> &v)
    {
        std::vector<std::string> r;
        for (const auto &a : v) r.push_back(a.path);
        std::sort(r.begin(), r.end());
        return r;
    }

    template <typename Action>
    std::vector<std::pair<std::string, std::string>> canonical(const std::vector<Action> &v)
    {
        std::vector<std::pair<std::string, std::string>> r;
        for (const auto &a : v) r.emplace_back(a.path, a.hash);
        std::sort(r.begin(), r.end());
        return r;
    }

    // The previous getDelta() entry rules, O(n·m): the reference for the merge-join.
    void referenceDiff(const std::vector<SimpleEntry> &source, const std::vector<SimpleEntry> &destination,
                       std::vector<AddAction> &adds, std::vector<RemoveAction> &removes)
    {
        for (const auto &entry : source)
        {
            const bool inDest = std::find_if(destination.begin(), destination.end(), [&](const SimpleEntry &e)
                                             { return e.hash == entry.hash && e.path == entry.path; }) != destination.end();
            if (!inDest) adds.emplace_back(entry.path, entry.hash);
        }
        for (const auto &entry : destination)
        {
            const bool inSource = std::find_if(source.begin(), source.end(), [&](const SimpleEntry &e)
                                               { return e.path == entry.path && e.isDirectory() == entry.isDirectory(); }) != source.end();
            if (!inSource) removes.emplace_back(entry.path, entry.hash);
        }
    }

    TEST(DDB03_Delta, semanticsAddsModifiedAndNew)
    {
        const auto src = makeStamp({{"a.jpg", "h1"}, {"b.jpg", "h2-new"}, {"c.jpg", "h3"}, {"d", ""}});
        const auto dst = makeStamp({{"a.jpg", "h1"}, {"b.jpg", "h2-old"}, {"d", ""}});
        const auto d = getDelta(src, dst);
        EXPECT_EQ(paths(d.adds), (std::vector<std::string>{"b.jpg", "c.jpg"}));
        EXPECT_TRUE(d.removes.empty());
    }

    TEST(DDB03_Delta, semanticsRemovesMissingAndTypeFlips)
    {
        // "x" is a folder in the destination and a file in the source: the
        // destination folder is removed and the source file is added.
        const auto src = makeStamp({{"x", "hfile"}});
        const auto dst = makeStamp({{"x", ""}, {"gone.jpg", "h9"}});
        const auto d = getDelta(src, dst);
        EXPECT_EQ(paths(d.adds), (std::vector<std::string>{"x"}));
        EXPECT_EQ(paths(d.removes), (std::vector<std::string>{"gone.jpg", "x"}));
    }

    TEST(DDB03_Delta, semanticsRemovesSortedDescending)
    {
        // applyDelta deletes children before parents: removes must be path-descending.
        const auto src = makeStamp({});
        const auto dst = makeStamp({{"f", ""}, {"f/a.jpg", "h"}, {"f/b.jpg", "h"}});
        const auto d = getDelta(src, dst);
        ASSERT_EQ(d.removes.size(), 3u);
        for (size_t i = 1; i < d.removes.size(); i++)
            EXPECT_GT(d.removes[i - 1].path, d.removes[i].path);
    }

    TEST(DDB03_Delta, semanticsMeta)
    {
        const auto src = makeStamp({}, {"m1", "m2"});
        const auto dst = makeStamp({}, {"m2", "m3"});
        const auto d = getDelta(src, dst);
        EXPECT_EQ(d.metaAdds, (std::vector<std::string>{"m1"}));
        EXPECT_EQ(d.metaRemoves, (std::vector<std::string>{"m3"}));
    }

    TEST(DDB03_Delta, semanticsUnsortedInput)
    {
        // Stamps from other tools are not necessarily sorted.
        const auto src = makeStamp({{"z.jpg", "h"}, {"a.jpg", "h"}, {"m.jpg", "new"}});
        const auto dst = makeStamp({{"m.jpg", "old"}, {"b.jpg", "h"}, {"z.jpg", "h"}});
        const auto d = getDelta(src, dst);
        EXPECT_EQ(paths(d.adds), (std::vector<std::string>{"a.jpg", "m.jpg"}));
        EXPECT_EQ(paths(d.removes), (std::vector<std::string>{"b.jpg"}));
    }

    TEST(DDB03_Delta, matchesReferenceImplementation)
    {
        std::mt19937 rng(20261019);
        const std::vector<std::string> names = {"a", "A", "a.jpg", "A.JPG", "b", "b/c.tif", "b/C.tif",
                                                "b/d", "b/d/e.las", "f g.txt", "ü.png", "z"};
        const std::vector<std::string> hashes = {"", "", "h1", "h2", "h3"};

        const auto randomEntries = [&](size_t n)
        {
            std::vector<SimpleEntry> v;
            for (size_t i = 0; i < n; i++)
                v.emplace_back(names[rng() % names.size()], hashes[rng() % hashes.size()]);
            return v;
        };
        const auto byPath = [](const SimpleEntry &l, const SimpleEntry &r)
        { return l.path < r.path; };

        for (int round = 0; round < 2000; round++)
        {
            // Duplicate paths, case variants, file/folder flips and empty sides all occur.
            auto src = randomEntries(rng() % 10);
            auto dst = randomEntries(rng() % 10);
            std::sort(src.begin(), src.end(), byPath);
            std::sort(dst.begin(), dst.end(), byPath);

            std::vector<AddAction> adds, refAdds;
            std::vector<RemoveAction> removes, refRemoves;
            diffSortedEntries(src, dst, adds, removes);
            referenceDiff(src, dst, refAdds, refRemoves);

            ASSERT_EQ(canonical(adds), canonical(refAdds)) << "round " << round;
            ASSERT_EQ(canonical(removes), canonical(refRemoves)) << "round " << round;
        }
    }

    // 1 % of the destination paths missing, 2 % modified
    std::pair<json, json> scalingStamps(int n)
    {
        std::vector<std::pair<std::string, std::string>> src, dst;
        src.reserve(n);
        dst.reserve(n);
        for (int i = 0; i < n; i++)
        {
            const std::string p = "dir" + std::to_string(i % 100) + "/IMG_" + std::to_string(i) + ".JPG";
            src.emplace_back(p, "abababab");
            if (i % 100 == 0) continue;
            dst.emplace_back(p, i % 50 == 0 ? "ffffffff" : "abababab");
        }
        return {makeStamp(src), makeStamp(dst)};
    }

    long long timeDelta(const json &s, const json &t, Delta &d)
    {
        const auto t0 = std::chrono::steady_clock::now();
        d = getDelta(s, t);
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0).count();
    }

    TEST(DDB03_Delta, isLinear)
    {
        if (std::getenv("DDB03_SKIP_TIMING") != nullptr)
            GTEST_SKIP() << "timing test disabled";

        const auto [s, t] = scalingStamps(50000);
        Delta d;
        const auto ms = timeDelta(s, t, d);

        EXPECT_EQ(d.adds.size(), 1000u);  // 500 missing in dst + 500 modified
        EXPECT_EQ(d.removes.size(), 0u);
        std::cout << "[ DDB03 ] getDelta(50k, 50k) took " << ms << " ms" << std::endl;
        EXPECT_LT(ms, 200) << "getDelta is quadratic (it took ~5 s before the merge-join)";
    }

    TEST(DDB03_Delta, isLinear200k)
    {
        if (std::getenv("DDB03_SKIP_TIMING") != nullptr)
            GTEST_SKIP() << "timing test disabled";

        const auto [s, t] = scalingStamps(200000);
        Delta d;
        const auto ms = timeDelta(s, t, d);

        EXPECT_EQ(d.adds.size(), 4000u);
        std::cout << "[ DDB03 ] getDelta(200k, 200k) took " << ms << " ms" << std::endl;
        EXPECT_LT(ms, 1000);
    }

    struct StampFixture
    {
        fs::path root;
        std::unique_ptr<Database> db;

        explicit StampFixture(const std::string &name)
        {
            TestArea ta(name, true);
            root = ta.getFolder("ds");
            initIndex(root.string());
            fs::create_directories(root / "dir");
            // Names that need escaping in JSON
            for (const auto &[rel, content] : std::vector<std::pair<std::string, std::string>>{
                     {"a.txt", "alpha"}, {"dir/b \"quoted\".txt", "bravo"},
                     {"dir/tab\tname.txt", "d"}, {"dir/ünïcödé.txt", "e"}})
            {
                std::ofstream f(root / rel);
                f << content;
            }
            db = ddb::open(root.string(), false);
            addToIndex(db.get(), {(root / "a.txt").string(), (root / "dir").string()});
            db->getMetaManager()->add("annotations", R"({"note":"x"})", "a.txt", root.string());
            db->getMetaManager()->add("annotations", R"({"note":"y"})", "", root.string());
        }
    };

    TEST(DDB03_Stamp, jsonMatchesDocument)
    {
        StampFixture f(TEST_NAME);
        const auto stamp = f.db->getStamp();
        ASSERT_EQ(stamp["entries"].size(), 5u);
        ASSERT_EQ(stamp["meta"].size(), 2u);

        EXPECT_EQ(f.db->getStampJson(), stamp.dump());
        EXPECT_EQ(json::parse(f.db->getStampJson()), stamp);
    }

    TEST(DDB03_Stamp, checksumMatchesStamp)
    {
        StampFixture f(TEST_NAME);
        const auto before = f.db->getStampChecksum();
        EXPECT_EQ(before, f.db->getStamp()["checksum"].get<std::string>());
        EXPECT_EQ(before.size(), 64u);

        std::ofstream(f.root / "new.txt") << "new";
        addToIndex(f.db.get(), {(f.root / "new.txt").string()});
        EXPECT_NE(f.db->getStampChecksum(), before);
        EXPECT_EQ(f.db->getStampChecksum(), f.db->getStamp()["checksum"].get<std::string>());
    }

    TEST(DDB03_Stamp, emptyIndex)
    {
        TestArea ta(TEST_NAME, true);
        const auto root = ta.getFolder("ds");
        initIndex(root.string());
        const auto db = ddb::open(root.string(), false);
        EXPECT_EQ(db->getStampJson(), db->getStamp().dump());
    }

    TEST(DDB03_Stamp, cApi)
    {
        StampFixture f(TEST_NAME);
        const auto path = f.root.string();
        f.db->close();

        char *out = nullptr;
        ASSERT_EQ(DDBGetStamp(path.c_str(), &out), DDBERR_NONE);
        const auto stamp = json::parse(out);
        DDBFree(out);

        char *checksum = nullptr;
        ASSERT_EQ(DDBGetStampChecksum(path.c_str(), &checksum), DDBERR_NONE);
        EXPECT_EQ(std::string(checksum), stamp["checksum"].get<std::string>());
        DDBFree(checksum);

        EXPECT_NE(DDBGetStampChecksum(path.c_str(), nullptr), DDBERR_NONE);
        EXPECT_NE(DDBGetStampChecksum("", &checksum), DDBERR_NONE);
    }
} // namespace
