/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-05: the shared string and math helpers.

#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "utils.h"

namespace {

using namespace ddb;

// join() removed the separator after the last item with pop_back(), also when there was no
// item: pop_back() on an empty string is undefined behaviour (libstdc++ sets its length to
// SIZE_MAX). Registry::downloadFiles joins its list unchecked.
TEST(Dq05Utilities, JoinOfNoItemsIsEmpty) {
    EXPECT_EQ(utils::join({}), "");
    EXPECT_EQ(utils::join({"a"}), "a");
    EXPECT_EQ(utils::join({"a", "b", ""}, ';'), "a;b;");
}

// split() with an empty delimiter looped forever: find("") matches at every position without
// moving on.
TEST(Dq05Utilities, SplitWithEmptyDelimiterReturnsTheWholeText) {
    EXPECT_EQ(utils::split("a,b", ""), std::vector<std::string>({"a,b"}));
}

}  // namespace
