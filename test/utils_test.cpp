/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "../utils.h"
#include "../logger.h"
#include <vector>
#include <string>

namespace {

TEST(PathsAreChildren, Normal) {
    EXPECT_TRUE(utils::pathsAreChildren("/my/path", {"/my/path/1", "/my/path"}));
    EXPECT_TRUE(utils::pathsAreChildren("path", {"path/1/2", "path/3", "path"}));
    EXPECT_TRUE(utils::pathsAreChildren("path/.", {"path/1/2", "path/3", "path"}));
    EXPECT_TRUE(utils::pathsAreChildren("path/./", {"path/./../path/"}));
    EXPECT_TRUE(utils::pathsAreChildren("path/./.", {"path/./../path"}));

    EXPECT_FALSE(utils::pathsAreChildren("path", {"test", "path/3", "path"}));
    EXPECT_FALSE(utils::pathsAreChildren("/my/path", {"/my/pat", "/my/path/1"}));
}

TEST(pathDepth, Normal) {
    EXPECT_EQ(utils::pathDepth(""), 0);
    EXPECT_EQ(utils::pathDepth("/"), 0);
    EXPECT_EQ(utils::pathDepth("/file.txt"), 0);
    EXPECT_EQ(utils::pathDepth("/a/file.txt"), 1);
    EXPECT_EQ(utils::pathDepth("/a/b/file.txt"), 2);
    EXPECT_EQ(utils::pathDepth("."), 0);
    EXPECT_EQ(utils::pathDepth("./."), 1);
}

}
