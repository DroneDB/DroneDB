/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "dbops.h"
#include "entry_path.h"
#include "exceptions.h"
#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"

namespace
{

    using namespace ddb;

    TEST(utilsTest, hasDotNotation)
    {

        EXPECT_FALSE(ddb::hasDotNotation("ciao/ciao"));
        EXPECT_FALSE(ddb::hasDotNotation("/ciao/"));
        EXPECT_FALSE(ddb::hasDotNotation("ciao/.a/a/tutti"));
        EXPECT_TRUE(ddb::hasDotNotation("ciao/./ciao"));
        EXPECT_TRUE(ddb::hasDotNotation("./ciao/./ciao"));
        EXPECT_TRUE(ddb::hasDotNotation("ciao/./ciao"));
        EXPECT_TRUE(ddb::hasDotNotation("ciao/../ciao"));
        EXPECT_TRUE(ddb::hasDotNotation("../ciao/.a./ciao"));
    }

}