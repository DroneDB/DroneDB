/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "dbops.h"
#include "exceptions.h"
#include "mio.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace {

using namespace ddb;

// NOTE (documented finding, see session notes / plan deviation log): the plan's "poison pill"
// exit criterion for Phase 3 - a batch with one corrupt/missing file committing the other n-1
// and reporting exactly one error - could not be exercised end-to-end through the public
// addToIndex/addToIndexEx entry points as written. Two independent, pre-existing behaviors
// (neither introduced by phases 1-3) block it:
//   1. getIndexPathList() (dbops.cpp) eagerly validates existence for every literal input path
//      *before* PLAN/COMPUTE ever run, and throws FSException for the whole call if any path is
//      missing - regardless of whether that path was previously indexed. This makes a
//      missing-file item a batch-scoped failure, not an item-scoped one, at the API boundary.
//   2. Every type-specific parser in entry.cpp (vector/model/tiles3d/georaster) is deliberately
//      "best-effort" and swallows its own parse errors (explicit comments to that effect), so
//      corrupt *content* of a recognized type never reaches computeAddEntries()'s per-item catch.
// computeAddEntries()'s per-item try/catch for FSException/GDALException/PDALException/
// JSONException/IndexException is still implemented and structurally correct (see
// stopOnErrorAbortsWholeBatch below for a related, verified behavior), but a genuine poison-pill
// regression test requires either loosening getIndexPathList's existence check to be per-item
// (a wider behavior change affecting syncIndex/rescanIndex/moveEntry/removeFromIndex, out of
// scope for this session - see 08-decision-log.md-style note in session memory) or a real
// propagating parser exception (none exist today). Flagged for the plan owner rather than
// silently faked.

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

// Every input path appears in exactly one of entries/unchanged/errors (completeness contract,
// 02-target-architecture.md §5.1) across repeated calls (second call finds good1.txt unchanged).
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

} // namespace
