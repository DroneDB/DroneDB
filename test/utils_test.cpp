/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "dbops.h"
#include "exceptions.h"
#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace {

using namespace ddb;

TEST(utilsTest, generateRandomString) {
    for (auto n = 0; n < 1000; n++) {
        auto str = utils::generateRandomString(100);
        // Uncomment for the WALL OF RANDOM
        // std::cout << str << std::endl;
        EXPECT_TRUE(str.length() == 100);
    }
}

}