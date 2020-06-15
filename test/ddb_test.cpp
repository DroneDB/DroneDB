/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "../libs/ddb.h"
#include "../classes/exceptions.h"

namespace {

using namespace ddb;

TEST(getIndexPathList, includeDirs) {
    auto pathList = ddb::getIndexPathList("data", {fs::path("data") / "folderA" / "test.txt"}, true);
    EXPECT_EQ(pathList.size(), 3);
    EXPECT_STREQ(pathList[0].c_str(), (fs::path("data") / "folderA" / "test.txt").c_str());
    EXPECT_STREQ(pathList[1].c_str(), fs::path("data").c_str());
    EXPECT_STREQ(pathList[2].c_str(), (fs::path("data") / "folderA").c_str());

    pathList = ddb::getIndexPathList(".", {
        fs::path("data") / "folderA" / "test.txt",
        fs::path("data") / "folderA" / "folderB" / "test.txt"
    }, true);
    EXPECT_EQ(pathList.size(), 5);
    EXPECT_STREQ(pathList[0].c_str(), (fs::path("data") / "folderA" / "test.txt").c_str());
    EXPECT_STREQ(pathList[1].c_str(), (fs::path("data") / "folderA" / "folderB" / "test.txt").c_str());
    EXPECT_STREQ(pathList[2].c_str(), (fs::path("data") / "folderA" / "folderB").c_str());
    EXPECT_STREQ(pathList[3].c_str(), (fs::path("data") / "folderA").c_str());
    EXPECT_STREQ(pathList[4].c_str(), fs::path("data").c_str());

    EXPECT_THROW(
    pathList = ddb::getIndexPathList("otherRoot", {
        fs::path("data") / "folderA" / "test.txt",
    }, true),
    FSException
    );

//    EXPECT_FALSE(utils::pathsAreChildren("/my/path", {"/my/pat", "/my/path/1"}));
}

TEST(getIndexPathList, dontIncludeDirs) {
    auto pathList = ddb::getIndexPathList("data", {fs::path("data") / "folderA" / "test.txt"}, false);
    EXPECT_EQ(pathList.size(), 1);
    EXPECT_STREQ(pathList[0].c_str(), (fs::path("data") / "folderA" / "test.txt").c_str());

}

}
