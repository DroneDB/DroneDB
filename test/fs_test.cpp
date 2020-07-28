/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "fs.h"
#include "logger.h"
#include <vector>
#include <string>

namespace {

using namespace ddb;

TEST(pathsAreChildren, Normal) {
    EXPECT_TRUE(pathsAreChildren("/my/path", { "/my/path/1", "/my/path/a/b/.." }));

#ifdef _WIN32
    EXPECT_TRUE(pathsAreChildren("C:\\my\\path", { "C:\\my\\path\\1", "C:\\my\\path\\a\\b\\.." }));
#endif

    EXPECT_TRUE(pathsAreChildren("path", {"path/1/2", "path/3", "path/././6"}));
    EXPECT_TRUE(pathsAreChildren("path/./", { "path/1/2", "path/3/", "path/./6/7/../" }));

#ifdef _WIN32
    EXPECT_TRUE(pathsAreChildren("path\\.", { "path\\1\\2", "path\\3", "path\\4\\" }));
#endif

    EXPECT_TRUE(pathsAreChildren("path/./", {"path/./../path/a/"}));
    EXPECT_TRUE(pathsAreChildren("path/./.", {"path/./../path/b"}));

    EXPECT_FALSE(pathsAreChildren("path", {"path/3", "path/a/.."}));
    EXPECT_FALSE(pathsAreChildren("/my/path", {"/my/pat", "/my/path/1"}));
}

TEST(pathDepth, Normal) {
	EXPECT_EQ(pathDepth(fs::path("")), 0);

#ifdef _WIN32
	EXPECT_EQ(pathDepth(fs::path("\\")), 0);
#else
	EXPECT_EQ(pathDepth(fs::path("/")), 0);
#endif

	EXPECT_EQ(pathDepth(fs::current_path().root_path()), 0); // C:\ or /
    EXPECT_EQ(pathDepth((fs::current_path().root_path() / "file.txt").string()), 0);
	EXPECT_EQ(pathDepth((fs::current_path().root_path() / "a" / "file.txt").string()), 1);
	EXPECT_EQ(pathDepth((fs::current_path().root_path() / "a" / "b" / "file.txt").string()), 2);
    EXPECT_EQ(pathDepth(fs::path(".")), 0);
    EXPECT_EQ(pathDepth(fs::path(".") / "."), 1);
}

TEST(pathIsChild, Normal){
    EXPECT_TRUE(pathIsChild(fs::path("/data/drone"), fs::path("/data/drone/a")));
    EXPECT_FALSE(pathIsChild(fs::path("/data/drone"), fs::path("/data/drone/")));
    EXPECT_FALSE(pathIsChild(fs::path("/data/drone"), fs::path("/data/drone")));
    EXPECT_FALSE(pathIsChild(fs::path("/data/drone/"), fs::path("/data/drone")));
    EXPECT_TRUE(pathIsChild(fs::path("data/drone"), fs::path("data/drone/123")));
    EXPECT_FALSE(pathIsChild(fs::path("data/drone"), fs::path("data/drone/123/..")));
    EXPECT_FALSE(pathIsChild(fs::path("data/drone"), fs::path("data/drone/123/./../")));
    EXPECT_FALSE(pathIsChild(fs::path("data/drone"), fs::path("data/drone/123/./../..")));
    EXPECT_TRUE(pathIsChild(fs::path("data/drone/a/.."), fs::path("data/drone/123")));
}

TEST(getRelPath, Normal) {
	EXPECT_EQ(getRelPath(fs::path("/home/test/aaa"), fs::path("/home/test")).generic_string(), fs::path("aaa").generic_string());
#ifdef _WIN32
	EXPECT_EQ(getRelPath(fs::path("D:/home/test/aaa"), fs::path("/")).generic_string(), fs::path("D:/home/test/aaa").generic_string());
#else
	EXPECT_EQ(getRelPath(fs::path("/home/test/aaa"), fs::path("/")).generic_string(), fs::path("home/test/aaa").generic_string());
#endif
	
	EXPECT_EQ(getRelPath(fs::path("/home/test/aaa/bbb/ccc/../.."), fs::path("/home")).generic_string(), fs::path("test/aaa").generic_string());

#ifdef _WIN32
	EXPECT_EQ(getRelPath(fs::path("D:\\"), fs::path("/")).generic_string(), fs::path("D:\\").generic_string());
#else
	EXPECT_EQ(getRelPath(fs::path("/"), fs::path("/")).generic_string(), fs::path(".").generic_string());
#endif
	EXPECT_EQ(getRelPath(fs::path("/"), fs::path("/a/..")).generic_string(), fs::path(".").generic_string());

#ifdef _WIN32
	EXPECT_EQ(getRelPath(fs::path("C:\\test"), fs::path("/")).generic_string(), fs::path("C:\\test").generic_string());
	EXPECT_EQ(getRelPath(fs::path("D:\\test\\..\\aaa"), fs::path("D:\\")).generic_string(), fs::path("aaa").generic_string());
#endif
}

}
