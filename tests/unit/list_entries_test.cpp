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
#include "utf8.h"
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
        ASSERT_EQ(DDBList(ddbPath.c_str(), paths, 1, &listed, "json", true, 0), DDBERR_NONE);
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

    // XI-09: letters match regardless of case for every script (Unicode simple case folding),
    // not only ASCII; a name or pattern that is not valid UTF-8 is compared byte for byte, with
    // ASCII letters in any case (as before)
    TEST(XI09_NamePatternCaseFolding, foldsNonAsciiLetters) {
        EXPECT_TRUE(utils::matchesNamePattern("Überflug.JPG", "über*.jpg"));
        EXPECT_TRUE(utils::matchesNamePattern("überflug.jpg", "ÜBER*"));
        EXPECT_TRUE(utils::matchesNamePattern("ÉTÉ.tif", "été.TIF"));
        EXPECT_TRUE(utils::matchesNamePattern("ΑΘΗΝΑ_01.jpg", "αθηνα_??.jpg"));
        EXPECT_TRUE(utils::matchesNamePattern("ΟΔΟΣ.txt", "οδος.txt"))
            << "final sigma folds to sigma";
        EXPECT_TRUE(utils::matchesNamePattern("МОСКВА.las", "*москва*"));
        EXPECT_TRUE(utils::matchesNamePattern("Straße", "STRAẞE"))
            << "U+1E9E folds to U+00DF (simple folding; the full folding to \"ss\" is not used)";
        EXPECT_TRUE(
            utils::matchesNamePattern("\xE2\x84\xAA"
                                      "elvin.txt",
                                      "kelvin.*"))
            << "KELVIN SIGN folds to k";
        EXPECT_TRUE(utils::matchesNamePattern("a\xC5\xBF.txt", "AS.TXT")) << "LONG S folds to s";
        EXPECT_TRUE(utils::matchesNamePattern("Ü", "?")) << "'?' is one character";

        EXPECT_FALSE(utils::matchesNamePattern("über", "uber")) << "no accent stripping";
        EXPECT_FALSE(utils::matchesNamePattern("Straße", "STRASSE")) << "no full folding";
        EXPECT_FALSE(utils::matchesNamePattern("ΑΘΗΝΑ.jpg", "αθηνα"));
        EXPECT_FALSE(utils::matchesNamePattern("ÜÜ", "?"));
    }

    TEST(XI09_NamePatternCaseFolding, simpleCaseFoldingTable) {
        EXPECT_EQ(utf8::foldCase(U'A'), U'a');
        EXPECT_EQ(utf8::foldCase(U'a'), U'a');
        EXPECT_EQ(utf8::foldCase(U'1'), U'1');
        EXPECT_EQ(utf8::foldCase(U'\u00C9'), U'\u00E9');          // É
        EXPECT_EQ(utf8::foldCase(U'\u03A3'), U'\u03C3');          // Σ
        EXPECT_EQ(utf8::foldCase(U'\u03C2'), U'\u03C3');          // ς (status C)
        EXPECT_EQ(utf8::foldCase(U'\u1E9E'), U'\u00DF');          // ẞ (status S; F would give "ss")
        EXPECT_EQ(utf8::foldCase(U'\u212A'), U'k');               // KELVIN SIGN
        EXPECT_EQ(utf8::foldCase(U'\U00010400'), U'\U00010428');  // Deseret, 4 UTF-8 bytes
        EXPECT_EQ(utf8::foldCase(U'\u0130'), U'\u0130');          // İ: statuses F and T only
        EXPECT_EQ(utf8::foldCase(U'\u0131'), U'\u0131');          // ı
        EXPECT_EQ(utf8::foldCase(U'\u6771'), U'\u6771');          // 東: no case

        std::u32string folded;
        EXPECT_TRUE(utf8::foldedCodePoints("ÜBER", folded));
        EXPECT_EQ(folded, U"\u00FCber");
        EXPECT_FALSE(utf8::foldedCodePoints("a\xFF", folded));

        EXPECT_TRUE(utf8::hasNonAsciiFoldVariant('K'));
        EXPECT_TRUE(utf8::hasNonAsciiFoldVariant('s'));
        EXPECT_FALSE(utf8::hasNonAsciiFoldVariant('a'));
        EXPECT_FALSE(utf8::hasNonAsciiFoldVariant('\0'));
    }

    TEST(XI09_NamePatternCaseFolding, invalidUtf8ComparesBytes) {
        EXPECT_TRUE(
            utils::matchesNamePattern("\xFF"
                                      "photo.JPG",
                                      "*.jpg"));
        EXPECT_TRUE(utils::matchesNamePattern("\xFF", "\xFF"));
        EXPECT_TRUE(utils::matchesNamePattern("\xC3", "?"));
        EXPECT_TRUE(
            utils::matchesNamePattern("\xFF"
                                      "ü",
                                      "*ü"));
        EXPECT_FALSE(
            utils::matchesNamePattern("\xFF"
                                      "Ü",
                                      "*ü"))
            << "no folding without valid UTF-8";
        EXPECT_FALSE(utils::matchesNamePattern("Ü", "\xFF"));
    }

    // The index query selects the case variants too (its LIKE pre-filter folds ASCII only). The
    // rows are inserted directly: Windows converts narrow file names with the ANSI code page.
    TEST(XI09_NamePatternCaseFolding, listEntriesSelectsNonAsciiCaseVariants) {
        TestArea area(TEST_NAME, true);
        const fs::path root = area.getFolder("ds");
        initIndex(root.string());
        {
            const auto db = ddb::open(root.string(), false);
            auto q = db->query(
                "INSERT INTO entries (path, hash, type, properties, mtime, size, depth) "
                "VALUES (?, '', 2, '{}', 0, 0, 0)");
            for (const std::string path : {"Überflug.txt",
                                           "überflug2.txt",
                                           "uberflug.txt",
                                           "ÉTÉ.txt",
                                           "МОСКВА.txt",
                                           "\xE2\x84\xAA"
                                           "elvin.txt",
                                           "kelvin.txt",
                                           "a\xC5\xBF.txt",
                                           "as.txt"}) {
                q->bind(1, path);
                q->execute();
            }
        }

        const auto list = [&root](const char* pattern) {
            char* found = nullptr;
            EXPECT_EQ(DDBListEntries(root.string().c_str(), "", true, pattern, "", false, &found),
                      DDBERR_NONE)
                << DDBGetLastError();
            Paths result;
            for (const auto& e : json::parse(found != nullptr ? found : "[]"))
                result.push_back(e["path"].get<std::string>());
            DDBFree(found);
            return result;
        };

        EXPECT_EQ(list("ÜBERFLUG*"), (Paths{"Überflug.txt", "überflug2.txt"}));
        EXPECT_EQ(list("*été*"), (Paths{"ÉTÉ.txt"}));
        EXPECT_EQ(list("москва.TXT"), (Paths{"МОСКВА.txt"}));
        // ASCII letters of the pattern that a non-ASCII letter folds to as well
        EXPECT_EQ(list("KELVIN.txt"),
                  (Paths{"kelvin.txt",
                         "\xE2\x84\xAA"
                         "elvin.txt"}));
        EXPECT_EQ(list("AS.*"), (Paths{"as.txt", "a\xC5\xBF.txt"}));
        DDBReleaseConnections(root.string().c_str(), nullptr);
    }
} // namespace
