/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-09: walkDataset is the one walker of dataset folder trees. These tests pin what the three
// walkers it replaced (getIndexPathList, getPathList, expandGlobPatterns) agreed on.

#include <algorithm>
#include <set>

#include "gtest/gtest.h"

#ifndef _WIN32
#include <unistd.h> // geteuid
#endif

#include "dataset_walk.h"
#include "dbops.h"
#include "exceptions.h"
#include "mio.h"
#include "status.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace
{
    using namespace ddb;

    std::set<std::string> relativeNames(const fs::path &root, const std::vector<fs::path> &paths)
    {
        std::set<std::string> names;
        for (const auto &p : paths)
            names.insert(io::Path(p).relativeTo(root).generic());
        return names;
    }

    std::set<std::string> walked(const fs::path &folder, const WalkOptions &options = {})
    {
        std::vector<fs::path> paths;
        walkDataset(folder, options, [&](const fs::directory_entry &entry) { paths.push_back(entry.path()); });
        return relativeNames(folder, paths);
    }

    // root/{a.txt, d1/{b.txt, d2/{c.txt}}, .ddb/{dbase.sqlite}, d1/.ddb/x}
    fs::path makeTree(const std::string &area)
    {
        TestArea ta(area, true);
        const auto root = ta.getFolder("tree");
        writeFile(root / "a.txt", "a");
        writeFile(root / "d1" / "b.txt", "b");
        writeFile(root / "d1" / "d2" / "c.txt", "c");
        writeFile(root / ".ddb" / "dbase.sqlite", "x");
        writeFile(root / "d1" / ".ddb" / "x", "x");
        return root;
    }

    TEST(Dq09Walk, ListsTheWholeTreeWithoutTheFolderItselfAndWithoutDdbFolders)
    {
        const auto root = makeTree(TEST_NAME);
        EXPECT_EQ(walked(root), (std::set<std::string>{"a.txt", "d1", "d1/b.txt", "d1/d2", "d1/d2/c.txt"}));
    }

    TEST(Dq09Walk, LevelsLimitTheDepthBelowTheStartFolder)
    {
        const auto root = makeTree(TEST_NAME);
        EXPECT_EQ(walked(root, {1}), (std::set<std::string>{"a.txt", "d1"}));
        EXPECT_EQ(walked(root, {2}), (std::set<std::string>{"a.txt", "d1", "d1/b.txt", "d1/d2"}));
        EXPECT_EQ(walked(root, {3}), walked(root, {0}));
    }

    TEST(Dq09Walk, ANonExistingFolderIsAnFsException)
    {
        const auto root = makeTree(TEST_NAME);
        EXPECT_THROW(walked(root / "missing"), FSException);
    }

    TEST(Dq09Walk, GetPathListMapsMaxDepthToLevels)
    {
        const auto root = makeTree(TEST_NAME);
        const auto list = [&](int maxDepth, bool dirs, bool files)
        { return relativeNames(root, getPathList({root.string()}, dirs, maxDepth, files)); };

        EXPECT_EQ(list(0, true, true), walked(root));
        EXPECT_EQ(list(1, true, true), (std::set<std::string>{"a.txt", "d1"}));
        EXPECT_EQ(list(-1, true, true), (std::set<std::string>{"a.txt", "d1"})) << "-1 lists the children only";
        EXPECT_EQ(list(-2, true, true), walked(root)) << "any other negative value lists everything";
        EXPECT_EQ(list(0, false, true), (std::set<std::string>{"a.txt", "d1/b.txt", "d1/d2/c.txt"}));
        EXPECT_EQ(list(0, true, false), (std::set<std::string>{"d1", "d1/d2"}));
    }

    TEST(Dq09Walk, GetIndexPathListWithAndWithoutFolders)
    {
        const auto root = makeTree(TEST_NAME);
        EXPECT_EQ(relativeNames(root, getIndexPathList(root, {root.string()}, true)),
                  (std::set<std::string>{"a.txt", "d1", "d1/b.txt", "d1/d2", "d1/d2/c.txt"}));
        EXPECT_EQ(relativeNames(root, getIndexPathList(root, {(root / "d1").string()}, true, false)),
                  (std::set<std::string>{"d1"}));
    }

    TEST(Dq09Walk, ExpandGlobPatternsOfAFolderSkipsDdbFolders)
    {
        const auto root = makeTree(TEST_NAME);
        std::vector<fs::path> paths;
        for (const auto &p : expandGlobPatterns({root.string()}))
            paths.push_back(io::fromUtf8(p));
        const auto names = relativeNames(root, paths);
        EXPECT_TRUE(names.count("d1/d2/c.txt"));
        EXPECT_FALSE(names.count(".ddb"));
        EXPECT_FALSE(names.count(".ddb/dbase.sqlite"));
        EXPECT_FALSE(names.count("d1/.ddb"));
    }

    TEST(Dq09Walk, TheIndexableNameRule)
    {
        EXPECT_TRUE(hasIndexableName("a/b.txt"));
        EXPECT_TRUE(hasIndexableName("a/.hidden"));
        EXPECT_FALSE(hasIndexableName("a/back\\slash.txt"));
        EXPECT_TRUE(hasIndexableName("back\\dir/file.txt")) << "only the name is checked, not the folders";
    }

    // ----------------------------------------------------------------------------------------
    // `ddb status` and `ddb add` look at the same tree: after an add, status has nothing to report
    // ----------------------------------------------------------------------------------------

    std::set<std::string> notIndexed(Database *db)
    {
        std::set<std::string> paths;
        statusIndex(db, [&](const FileStatus status, const std::string &file)
        {
            if (status == NotIndexed)
                paths.insert(file);
        });
        return paths;
    }

    void addTree(IndexFixture &f)
    {
        addToIndex(f.db.get(), expandGlobPatterns({f.root.string()}));
    }

    TEST(Dq09StatusParity, ADatasetAddedWholeHasNothingNotIndexed)
    {
        IndexFixture f(TEST_NAME);
        writeFile(f.root / "a.txt", "a");
        writeFile(f.root / "sub" / ".hidden", "h");
        writeFile(f.root / "sub" / "sp ace #%.txt", "s");
        addTree(f);
        EXPECT_TRUE(notIndexed(f.db.get()).empty());
    }

    TEST(Dq09StatusParity, StatusReportsAFileThatWasNotAdded)
    {
        IndexFixture f(TEST_NAME);
        writeFile(f.root / "a.txt", "a");
        addTree(f);
        writeFile(f.root / "later.txt", "l");
        EXPECT_EQ(notIndexed(f.db.get()), (std::set<std::string>{"later.txt"}));
    }

#ifndef _WIN32
    // A name with a backslash is legal on POSIX but cannot be an entry path: add skips the file
    // (planAddCandidates), so status must not list it as waiting to be added forever
    TEST(Dq09StatusParity, AFileNameWithABackslashIsNotReportedAsNotIndexed)
    {
        IndexFixture f(TEST_NAME);
        writeFile(f.root / "a.txt", "a");
        writeFile(f.root / "sub" / "back\\slash.txt", "b");
        addTree(f);
        EXPECT_FALSE(pathExists(f.db.get(), "sub/back\\slash.txt")) << "add never indexes it";
        EXPECT_TRUE(notIndexed(f.db.get()).empty());
    }

    // add skips the folders it cannot read (skip_permission_denied); status stopped with an
    // error on the first one
    TEST(Dq09StatusParity, AFolderTheProcessCannotReadIsSkippedLikeAddDoes)
    {
        if (geteuid() == 0)
            GTEST_SKIP() << "root reads every folder";

        IndexFixture f(TEST_NAME);
        writeFile(f.root / "a.txt", "a");
        writeFile(f.root / "locked" / "inside.txt", "i");
        fs::permissions(f.root / "locked", fs::perms::none);

        std::set<std::string> reported;
        try
        {
            addTree(f);
            reported = notIndexed(f.db.get());
        }
        catch (...)
        {
            fs::permissions(f.root / "locked", fs::perms::owner_all);
            throw;
        }
        fs::permissions(f.root / "locked", fs::perms::owner_all);

        EXPECT_TRUE(pathExists(f.db.get(), "locked")) << "the folder itself is an entry";
        EXPECT_FALSE(pathExists(f.db.get(), "locked/inside.txt"));
        EXPECT_TRUE(reported.empty());
    }
#endif
}
