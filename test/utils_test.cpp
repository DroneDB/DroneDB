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
	EXPECT_TRUE(utils::pathsAreChildren("/my/path", { "/my/path/1", "/my/path" }));
	EXPECT_TRUE(utils::pathsAreChildren("C:\\my\\path", { "C:\\my\\path\\1", "C:\\my\\path" }));

    EXPECT_TRUE(utils::pathsAreChildren("path", {"path/1/2", "path/3", "path"}));
	EXPECT_TRUE(utils::pathsAreChildren("path/.", { "path/1/2", "path/3", "path" }));
	EXPECT_TRUE(utils::pathsAreChildren("path\\.", { "path\\1\\2", "path\\3", "path" }));
    EXPECT_TRUE(utils::pathsAreChildren("path/./", {"path/./../path/"}));
    EXPECT_TRUE(utils::pathsAreChildren("path/./.", {"path/./../path"}));

    EXPECT_FALSE(utils::pathsAreChildren("path", {"test", "path/3", "path"}));
    EXPECT_FALSE(utils::pathsAreChildren("/my/path", {"/my/pat", "/my/path/1"}));
}

TEST(pathDepth, Normal) {
	EXPECT_EQ(utils::pathDepth(fs::path("")), 0);

#ifdef _WIN32
	EXPECT_EQ(utils::pathDepth(fs::path("\\")), 0);
#else
	EXPECT_EQ(utils::pathDepth(fs::path("/")), 0);
#endif

	EXPECT_EQ(utils::pathDepth(fs::current_path().root_path()), 0); // C:\ or /
    EXPECT_EQ(utils::pathDepth((fs::current_path().root_path() / "file.txt").string()), 0);
	EXPECT_EQ(utils::pathDepth((fs::current_path().root_path() / "a" / "file.txt").string()), 1);
	EXPECT_EQ(utils::pathDepth((fs::current_path().root_path() / "a" / "b" / "file.txt").string()), 2);
    EXPECT_EQ(utils::pathDepth(fs::path(".")), 0);
    EXPECT_EQ(utils::pathDepth(fs::path(".") / "."), 1);
}

}
