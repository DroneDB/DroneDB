/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-10 (XI-03): ddb.h as C sees it. c11_header.c includes ddb.h in a C11 translation unit built
// with warnings as errors (tests/CMakeLists.txt); here its layout of the public types must equal
// C++'s, and calls made from C behave as documented.

#include <cstddef>

#include "c11_header.h"
#include "ddb.h"
#include "gtest/gtest.h"

namespace {

TEST(CHeader, layoutMatchesCxx) {
    const DDBTestCLayout c = ddbTestCLayout();
    EXPECT_EQ(c.boolSize, sizeof(bool));
    EXPECT_EQ(c.boolSize, 1u);
    EXPECT_EQ(c.errSize, sizeof(DDBErr));
    EXPECT_EQ(c.errSize, 4u);
    EXPECT_EQ(c.addOptionsSize, sizeof(DDBAddOptions));
    EXPECT_EQ(c.addOptionsStopOnError, offsetof(DDBAddOptions, stopOnError));
    EXPECT_EQ(c.addOptionsMaxConflictRetries, offsetof(DDBAddOptions, maxConflictRetries));
    EXPECT_EQ(c.addHintSize, sizeof(DDBAddHint));
    EXPECT_EQ(c.addHintSha256, offsetof(DDBAddHint, sha256));
    EXPECT_EQ(c.addHintFileSize, offsetof(DDBAddHint, size));
    EXPECT_EQ(c.addHintMtime, offsetof(DDBAddHint, mtime));
}

TEST(CHeader, callsFromC) {
    EXPECT_EQ(ddbTestCCalls(), 0) << "first failed check in c11_header.c; last error: " << DDBGetLastError();
}

}  // namespace
