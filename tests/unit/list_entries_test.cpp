/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// REG-08: listEntries()/DDBListEntries filter by folder, name pattern and type in the index
// query, and can skip properties, geometries and meta.

#include <string>
#include <vector>

#include "dbops.h"
#include "ddb.h"
#include "exceptions.h"
#include "gtest/gtest.h"
#include "json.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace
{
    using namespace ddb;

    TEST(REG08_NamePattern, matchesLikeASimpleExpression)
    {
        EXPECT_TRUE(utils::matchesNamePattern("photo.JPG", "*.jpg"));
        EXPECT_TRUE(utils::matchesNamePattern("photo.jpg", "PHOTO.*"));
        EXPECT_TRUE(utils::matchesNamePattern("photo.jpg", "*"));
        EXPECT_TRUE(utils::matchesNamePattern("", "*"));
        EXPECT_TRUE(utils::matchesNamePattern("a1.tif", "a?.tif"));
        EXPECT_TRUE(utils::matchesNamePattern("aé.tif", "a?.tif")) << "'?' is one UTF-8 character";
        EXPECT_TRUE(utils::matchesNamePattern("abcabd", "*abd"));
        EXPECT_TRUE(utils::matchesNamePattern("x_y%z", "x_y%z"));
        EXPECT_TRUE(utils::matchesNamePattern("photo", "p*o*o"));

        EXPECT_FALSE(utils::matchesNamePattern("photo.jpg", "*.tif"));
        EXPECT_FALSE(utils::matchesNamePattern("a12.tif", "a?.tif"));
        EXPECT_FALSE(utils::matchesNamePattern("photo.jpg", "photo"));
        EXPECT_FALSE(utils::matchesNamePattern("xay", "x_y"));
        EXPECT_FALSE(utils::matchesNamePattern("photo", ""));
    }

    class REG08_ListEntries : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            area = std::make_unique<TestArea>(TEST_NAME, true);
            root = area->getFolder("ds");
            for (const auto *file : {"a.jpg", "B.JPG", "notes.txt", "dir1/c.jpg", "dir1/sub/d.jpg", "dir1/sub/e.txt",
                                     "dir1x/f.jpg", "dir2/x_1.tif", "dir2/x%1.tif", "dir2/xa1.tif"})
            {
                fs::create_directories((root / file).parent_path());
                fileWriteAllText(root / file, file);
            }
            initIndex(root.string());
            db = ddb::open(root.string(), false);
            addToIndex(db.get(), {root.string()});
        }

        std::vector<std::string> paths(const EntryQuery &query)
        {
            std::vector<std::string> result;
            for (const auto &e : listEntries(db.get(), query))
                result.push_back(e.path);
            return result;
        }

        std::unique_ptr<TestArea> area;
        fs::path root;
        std::unique_ptr<Database> db;
    };

    using Paths = std::vector<std::string>;

    TEST_F(REG08_ListEntries, folderRecursiveAndChildren)
    {
        EntryQuery q;
        q.folder = "dir1";
        EXPECT_EQ(paths(q), (Paths{"dir1/c.jpg", "dir1/sub", "dir1/sub/d.jpg", "dir1/sub/e.txt"}))
            << "sibling dir1x is not below dir1";

        q.recursive = false;
        EXPECT_EQ(paths(q), (Paths{"dir1/c.jpg", "dir1/sub"}));

        q.folder = "";
        EXPECT_EQ(paths(q), (Paths{"B.JPG", "a.jpg", "dir1", "dir1x", "dir2", "notes.txt"}));

        q.folder = "./dir1/sub/";
        EXPECT_EQ(paths(q), (Paths{"dir1/sub/d.jpg", "dir1/sub/e.txt"}));

        q.folder = "dir1/c.jpg";
        EXPECT_EQ(paths(q), (Paths{"dir1/c.jpg"})) << "a file selects itself";

        q.folder = "missing";
        EXPECT_TRUE(paths(q).empty());

        q.folder = "../outside";
        EXPECT_THROW(paths(q), InvalidArgsException);
    }

    TEST_F(REG08_ListEntries, namePattern)
    {
        EntryQuery q;
        q.namePattern = "*.jpg";
        EXPECT_EQ(paths(q), (Paths{"B.JPG", "a.jpg", "dir1/c.jpg", "dir1/sub/d.jpg", "dir1x/f.jpg"}));

        q.folder = "dir1";
        EXPECT_EQ(paths(q), (Paths{"dir1/c.jpg", "dir1/sub/d.jpg"}));

        q.recursive = false;
        EXPECT_EQ(paths(q), (Paths{"dir1/c.jpg"}));

        // '_' and '%' are literal, '?' is one character; folder names never match
        q = {};
        q.namePattern = "x_1.tif";
        EXPECT_EQ(paths(q), (Paths{"dir2/x_1.tif"}));
        q.namePattern = "x%1.tif";
        EXPECT_EQ(paths(q), (Paths{"dir2/x%1.tif"}));
        q.namePattern = "x?1.tif";
        EXPECT_EQ(paths(q), (Paths{"dir2/x%1.tif", "dir2/x_1.tif", "dir2/xa1.tif"}));
        q.namePattern = "dir*";
        EXPECT_EQ(paths(q), (Paths{"dir1", "dir1x", "dir2"}));
        q.namePattern = "*sub*";
        EXPECT_EQ(paths(q), (Paths{"dir1/sub"})) << "only the name, not the folders above it, is matched";
    }

    TEST_F(REG08_ListEntries, typesAndDetails)
    {
        EntryQuery q;
        q.types = {EntryType::Directory};
        EXPECT_EQ(paths(q), (Paths{"dir1", "dir1/sub", "dir1x", "dir2"}));

        q.types = {EntryType::Generic};
        q.folder = "dir1";
        EXPECT_EQ(paths(q), (Paths{"dir1/sub/e.txt"}));

        q.types = {EntryType::Generic, EntryType::Image};
        EXPECT_EQ(paths(q), (Paths{"dir1/c.jpg", "dir1/sub/d.jpg", "dir1/sub/e.txt"}));

        // Details match getEntryWithMeta; without them only the summary fields are set
        q = {};
        q.folder = "dir1/c.jpg";
        Entry expected;
        ASSERT_TRUE(getEntryWithMeta(db.get(), "dir1/c.jpg", expected));
        const auto full = listEntries(db.get(), q);
        ASSERT_EQ(full.size(), 1u);
        EXPECT_EQ(full[0].toJSONString(), expected.toJSONString());

        q.withDetails = false;
        const auto lean = listEntries(db.get(), q);
        ASSERT_EQ(lean.size(), 1u);
        json j;
        lean[0].toJSON(j);
        EXPECT_EQ(j, (json{{"path", "dir1/c.jpg"}, {"hash", expected.hash}, {"type", expected.type},
                           {"mtime", expected.mtime}, {"size", expected.size}, {"depth", 1}}));
    }

    TEST_F(REG08_ListEntries, cApiMatchesDdbList)
    {
        // Full details: the same JSON as DDBList for the same folder
        const auto ddbPath = root.string();
        const auto dir = (root / "dir1").string();
        const char *paths[] = {dir.c_str()};
        char *listed = nullptr;
        ASSERT_EQ(DDBList(ddbPath.c_str(), paths, 1, &listed, "json", true), DDBERR_NONE);
        char *found = nullptr;
        ASSERT_EQ(DDBListEntries(ddbPath.c_str(), "dir1", true, "", "", true, &found), DDBERR_NONE);
        EXPECT_EQ(json::parse(found), json::parse(listed));
        DDBFree(listed);
        DDBFree(found);

        ASSERT_EQ(DDBListEntries(ddbPath.c_str(), "", true, "*.JPG", "image,directory", false, &found), DDBERR_NONE);
        const auto j = json::parse(found);
        DDBFree(found);
        ASSERT_EQ(j.size(), 5u);
        EXPECT_FALSE(j[0].contains("properties"));

        EXPECT_EQ(DDBListEntries(ddbPath.c_str(), "", true, "", "nosuchtype", false, &found), DDBERR_EXCEPTION);
    }
} // namespace
