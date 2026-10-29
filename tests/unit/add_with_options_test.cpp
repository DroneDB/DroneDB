/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <algorithm>

#include "gtest/gtest.h"
#include "dbops.h"
#include "exceptions.h"
#include "mio.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace {

using namespace ddb;

// NOTE (documented finding): the "poison pill" behavior - a batch with one corrupt/missing
// file committing the other n-1 and reporting exactly one error - could not be exercised
// end-to-end through the public addToIndex/addToIndexEx entry points as written. Two
// independent, pre-existing behaviors block it:
//   1. getIndexPathList() (path_expansion.cpp) eagerly validates existence for every literal input path
//      *before* PLAN/COMPUTE ever run, and throws FSException for the whole call if any path is
//      missing - regardless of whether that path was previously indexed. This makes a
//      missing-file item a batch-scoped failure, not an item-scoped one, at the API boundary.
//   2. Every type-specific parser in entry.cpp (vector/model/tiles3d/georaster) is deliberately
//      "best-effort" and swallows its own parse errors (explicit comments to that effect), so
//      corrupt *content* of a recognized type never reaches computeAddEntries()'s per-item catch.
// computeAddEntries()'s per-item try/catch for FSException/GDALException/PDALException/
// JSONException/IndexException is still implemented and structurally correct (see
// stopOnErrorAbortsWholeBatch below for a related, verified behavior), but a genuine poison-pill
// regression test would require either loosening getIndexPathList's existence check to be
// per-item (a wider behavior change affecting
// syncIndex/rescanIndex/moveEntry/removeFromIndex) or a real propagating parser exception (none
// exist today).

// With stopOnError == true (the default, matching addToIndex()'s legacy semantics), a single
// item failure still aborts the whole call and no rows are committed.
TEST(addToIndexEx, stopOnErrorAbortsWholeBatch) {
    TestArea ta(TEST_NAME, true);
    const auto root = ta.getFolder("ds");

    fileWriteAllText(root / "good1.txt", "hello");
    const auto missing = (root / "missing.txt").string();

    initIndex(root.string());
    auto db = ddb::open(root.string(), true);

    AddOptions opts; // stopOnError defaults to true
    AddResult result;

    EXPECT_THROW(
        addToIndexEx(db.get(), {(root / "good1.txt").string(), missing}, opts, result),
        FSException);

    // Nothing should have been committed.
    auto q = db->query("SELECT COUNT(*) FROM entries");
    q->fetch();
    EXPECT_EQ(q->getInt(0), 0);
}

// Every input path appears in exactly one of entries/unchanged/errors (completeness contract)
// across repeated calls (second call finds good1.txt unchanged).
TEST(addToIndexEx, completenessContractAcrossCalls) {
    TestArea ta(TEST_NAME, true);
    const auto root = ta.getFolder("ds");

    fileWriteAllText(root / "good1.txt", "hello");

    initIndex(root.string());
    auto db = ddb::open(root.string(), true);
    AddOptions opts;
    opts.stopOnError = false;

    AddResult first;
    addToIndexEx(db.get(), {(root / "good1.txt").string()}, opts, first);
    EXPECT_EQ(first.entries.size(), 1u);
    EXPECT_TRUE(first.unchanged.empty());
    EXPECT_TRUE(first.errors.empty());

    AddResult second;
    addToIndexEx(db.get(), {(root / "good1.txt").string()}, opts, second);
    EXPECT_TRUE(second.entries.empty());
    ASSERT_EQ(second.unchanged.size(), 1u);
    EXPECT_EQ(second.unchanged[0], "good1.txt");
    EXPECT_TRUE(second.errors.empty());
}

// A new entry is reported as added and a changed one as updated, in AddResult and to the
// callback (both used to carry the opposite flag, so `ddb add` printed "U" for new files).
TEST(addToIndexEx, reportsAddedThenUpdated) {
    TestArea ta(TEST_NAME, true);
    const auto root = ta.getFolder("ds");
    const auto file = root / "good1.txt";

    fileWriteAllText(file, "hello");

    initIndex(root.string());
    auto db = ddb::open(root.string(), true);
    AddOptions opts;

    std::vector<bool> reported;
    const auto callback = [&reported](const Entry &, bool updated) {
        reported.push_back(updated);
        return true;
    };

    AddResult first;
    addToIndexEx(db.get(), {file.string()}, opts, first, callback);
    ASSERT_EQ(first.entries.size(), 1u);
    EXPECT_FALSE(first.entries[0].second) << "a new entry is not an update";

    fileWriteAllText(file, "hello, changed");
    fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(10));

    AddResult second;
    addToIndexEx(db.get(), {file.string()}, opts, second, callback);
    ASSERT_EQ(second.entries.size(), 1u);
    EXPECT_TRUE(second.entries[0].second);

    EXPECT_EQ(reported, (std::vector<bool>{false, true}));
}

// The errors of result with the given path, as "code" strings
std::vector<std::string> errorCodesOf(const AddResult &result, const std::string &path) {
    std::vector<std::string> codes;
    for (const auto &e : result.errors) {
        if (e.path == path) codes.push_back(e.code);
    }
    return codes;
}

bool hasEntry(const AddResult &result, const std::string &path) {
    return std::any_of(result.entries.begin(), result.entries.end(),
                       [&path](const auto &e) { return e.first.path == path; });
}

#ifndef _WIN32
// A file whose name contains a backslash (a path separator for the index) cannot be indexed:
// it is reported as an error instead of vanishing from entries, unchanged and errors.
TEST(addToIndexEx, reportsExplicitFileWithSeparatorInName) {
    TestArea ta(TEST_NAME, true);
    const auto root = ta.getFolder("ds");

    fileWriteAllText(root / "a\\b.txt", "backslash");
    fileWriteAllText(root / "c.txt", "plain");

    initIndex(root.string());
    auto db = ddb::open(root.string(), true);
    AddOptions opts;
    opts.stopOnError = false;

    AddResult result;
    addToIndexEx(db.get(), {(root / "a\\b.txt").string(), (root / "c.txt").string()}, opts, result);

    EXPECT_TRUE(hasEntry(result, "c.txt"));
    EXPECT_EQ(result.entries.size(), 1u);
    EXPECT_TRUE(result.unchanged.empty());
    ASSERT_EQ(result.errors.size(), 1u);
    EXPECT_EQ(result.errors[0].path, "a\\b.txt");
    EXPECT_EQ(result.errors[0].code, "INDEX");
    EXPECT_NE(result.errors[0].message.find("rename"), std::string::npos) << result.errors[0].message;

    // Nothing was written for it
    auto q = db->query("SELECT COUNT(*) FROM entries");
    q->fetch();
    EXPECT_EQ(q->getInt(0), 1);
}

// With stopOnError the unindexable name aborts the call like any other item error, before
// anything is indexed.
TEST(addToIndexEx, stopOnErrorAbortsOnExplicitFileWithSeparatorInName) {
    TestArea ta(TEST_NAME, true);
    const auto root = ta.getFolder("ds");

    fileWriteAllText(root / "a\\b.txt", "backslash");
    fileWriteAllText(root / "c.txt", "plain");

    initIndex(root.string());
    auto db = ddb::open(root.string(), true);
    AddOptions opts; // stopOnError defaults to true
    AddResult result;

    EXPECT_THROW(addToIndexEx(db.get(), {(root / "c.txt").string(), (root / "a\\b.txt").string()}, opts, result),
                 IndexException);

    auto q = db->query("SELECT COUNT(*) FROM entries");
    q->fetch();
    EXPECT_EQ(q->getInt(0), 0);
}

// The same file found by walking a folder is skipped without an error, as before.
TEST(addToIndexEx, skipsFileWithSeparatorInNameFoundInFolder) {
    TestArea ta(TEST_NAME, true);
    const auto root = ta.getFolder("ds");
    const auto folder = root / "d";
    fs::create_directories(folder);

    fileWriteAllText(folder / "a\\b.txt", "backslash");
    fileWriteAllText(folder / "c.txt", "plain");

    initIndex(root.string());
    auto db = ddb::open(root.string(), true);
    AddOptions opts;
    opts.stopOnError = false;

    AddResult result;
    addToIndexEx(db.get(), {folder.string()}, opts, result);

    EXPECT_TRUE(result.errors.empty());
    EXPECT_TRUE(hasEntry(result, "d/c.txt"));
    EXPECT_FALSE(hasEntry(result, "d/a\\b.txt"));
}
#endif

// A listed path named .ddb is reserved: reported, not dropped.
TEST(addToIndexEx, reportsExplicitReservedName) {
    TestArea ta(TEST_NAME, true);
    const auto root = ta.getFolder("ds");

    fileWriteAllText(root / "c.txt", "plain");

    initIndex(root.string());
    auto db = ddb::open(root.string(), true);
    AddOptions opts;
    opts.stopOnError = false;

    AddResult result;
    addToIndexEx(db.get(), {(root / ".ddb").string(), (root / "c.txt").string()}, opts, result);

    EXPECT_TRUE(hasEntry(result, "c.txt"));
    ASSERT_EQ(result.errors.size(), 1u);
    EXPECT_EQ(result.errors[0].path, ".ddb");
    EXPECT_EQ(result.errors[0].code, "INDEX");
    EXPECT_NE(result.errors[0].message.find("reserved"), std::string::npos) << result.errors[0].message;
}

// A listed file below the .ddb folder is reserved too.
TEST(addToIndexEx, reportsExplicitPathInsideReservedFolder) {
    TestArea ta(TEST_NAME, true);
    const auto root = ta.getFolder("ds");

    initIndex(root.string());
    fs::create_directories(root / ".ddb" / "build");
    fileWriteAllText(root / ".ddb" / "build" / "x.tif", "not an image");
    auto db = ddb::open(root.string(), true);
    AddOptions opts;
    opts.stopOnError = false;

    AddResult result;
    addToIndexEx(db.get(), {(root / ".ddb" / "build" / "x.tif").string()}, opts, result);

    EXPECT_TRUE(result.entries.empty());
    EXPECT_TRUE(result.unchanged.empty());
    EXPECT_EQ(errorCodesOf(result, ".ddb/build/x.tif"), std::vector<std::string>{"INDEX"});
    ASSERT_EQ(result.errors.size(), 1u);
    EXPECT_NE(result.errors[0].message.find("reserved"), std::string::npos) << result.errors[0].message;
}

} // namespace
