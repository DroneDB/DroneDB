/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Guards the C#-compatibility contract of DDBAddWithOptions: with stopOnError == false, the
// per-item error bucket must carry dataset-relative paths (db root stripped) — exactly like the
// entries/unchanged buckets — because the Registry C# adapter keys its byPath result-matching on
// relative paths. Callers (Registry, Node bindings) pass absolute paths.
//
// The normalization lives in ddb.cpp (DDBAddWithOptions), so this test intentionally goes
// through the C API rather than the C++ addToIndexEx seam.

#include "ddb.h"
#include "gtest/gtest.h"
#include "json.h"
#include "mio.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace {

using namespace ddb;

TEST(DDBAddWithOptions, deletionBeforeCommitReportsRelativeErrorPath) {
    TestArea ta(TEST_NAME, true);
    const auto root = ta.getFolder("ds");

    char *outPath = nullptr;
    ASSERT_EQ(DDBInit(root.string().c_str(), &outPath), DDBERR_NONE);
    DDBFree(outPath);

    // Good payload files (parse and commit successfully).
    const auto good1 = root / "good1.txt";
    const auto good2 = root / "good2.txt";
    fileWriteAllText(good1, "one");
    fileWriteAllText(good2, "two");

    // The scenario's deleted file: created, then removed before the add. In recursive
    // expansion, expandPathList throws per-item for this missing non-glob path (getPathList),
    // which is the exact seam whose path normalization this test guards. (In NON-recursive mode
    // a missing path aborts the whole call in getIndexPathList with DDBERR_EXCEPTION, so this
    // per-item bucket is unreachable — that is why recursive must be true here.)
    const auto gone = root / "gone.txt";
    fileWriteAllText(gone, "three");
    ASSERT_TRUE(fs::remove(gone));

    const std::string good1Abs = good1.string();
    const std::string good2Abs = good2.string();
    const std::string goneAbs = gone.string();
    const char *paths[] = {good1Abs.c_str(), good2Abs.c_str(), goneAbs.c_str()};

    DDBAddOptions opts;
    opts.recursive = true; // required to reach the per-item expand-failure seam under test
    opts.stopOnError = false; // per-item isolation
    opts.maxConflictRetries = 2;

    char *out = nullptr;
    ASSERT_EQ(DDBAddWithOptions(root.string().c_str(), paths, 3, &opts, &out), DDBERR_NONE);

    const auto j = json::parse(out);
    DDBFree(out);

    // Both good files committed (entry paths are dataset-relative).
    ASSERT_EQ(j["entries"].size(), 2u);

    // The deleted file produced exactly one item-scoped error, carrying the DATASET-RELATIVE
    // path (db root stripped), not the caller-supplied absolute path.
    ASSERT_EQ(j["errors"].size(), 1u);
    EXPECT_EQ(j["errors"][0]["path"].get<std::string>(), "gone.txt");
    EXPECT_NE(j["errors"][0]["path"].get<std::string>(), goneAbs);
    EXPECT_FALSE(j["errors"][0]["message"].get<std::string>().empty());
}

} // namespace
