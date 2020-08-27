/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "dbops.h"
#include "exceptions.h"

namespace {

using namespace ddb;

TEST(getIndexPathList, includeDirs) {
    auto pathList = ddb::getIndexPathList("data", {(fs::path("data") / "folderA" / "test.txt").string()}, true);
    EXPECT_EQ(pathList.size(), 2);
	EXPECT_TRUE(std::find(pathList.begin(), pathList.end(), fs::path("data") / "folderA" / "test.txt") != pathList.end());
	EXPECT_TRUE(std::find(pathList.begin(), pathList.end(), fs::path("data") / "folderA") != pathList.end());

    pathList = ddb::getIndexPathList(".", {
		(fs::path("data") / "folderA" / "test.txt").string(),
        (fs::path("data") / "folderA" / "folderB" / "test.txt").string()}, true);
    EXPECT_EQ(pathList.size(), 5);
	EXPECT_TRUE(std::find(pathList.begin(), pathList.end(), fs::path("data") / "folderA" / "test.txt") != pathList.end());
	EXPECT_TRUE(std::find(pathList.begin(), pathList.end(), fs::path("data") / "folderA" / "folderB" / "test.txt") != pathList.end());
	EXPECT_TRUE(std::find(pathList.begin(), pathList.end(), fs::path("data") / "folderA") != pathList.end());
	EXPECT_TRUE(std::find(pathList.begin(), pathList.end(), fs::path("data")) != pathList.end());
	EXPECT_TRUE(std::find(pathList.begin(), pathList.end(), fs::path("data") / "folderA" / "folderB") != pathList.end());

    EXPECT_THROW(
    pathList = ddb::getIndexPathList("otherRoot", {
        (fs::path("data") / "folderA" / "test.txt").string(),
    }, true),
    FSException
    );
}

TEST(getIndexPathList, dontIncludeDirs) {
    auto pathList = ddb::getIndexPathList("data", {(fs::path("data") / "folderA" / "test.txt").string()}, false);
    EXPECT_EQ(pathList.size(), 1);
    EXPECT_STREQ(pathList[0].string().c_str(), (fs::path("data") / "folderA" / "test.txt").string().c_str());

}

}
