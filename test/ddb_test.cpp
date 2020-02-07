/* Copyright 2019 MasseranoLabs LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include "gtest/gtest.h"
#include "../libs/ddb.h"
#include "../classes/exceptions.h"

namespace {

using namespace ddb;

TEST(getPathList, includeDirs) {
    auto pathList = ddb::getPathList("data", {fs::path("data") / "folderA" / "test.txt"}, true);
    EXPECT_EQ(pathList.size(), 3);
    EXPECT_STREQ(pathList[0].c_str(), (fs::path("data") / "folderA" / "test.txt").c_str());
    EXPECT_STREQ(pathList[1].c_str(), fs::path("data").c_str());
    EXPECT_STREQ(pathList[2].c_str(), (fs::path("data") / "folderA").c_str());

    pathList = ddb::getPathList(".", {
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
    pathList = ddb::getPathList("otherRoot", {
        fs::path("data") / "folderA" / "test.txt",
    }, true),
    FSException
    );

//    EXPECT_FALSE(utils::pathsAreChildren("/my/path", {"/my/pat", "/my/path/1"}));
}

TEST(getPathList, dontIncludeDirs) {
    auto pathList = ddb::getPathList("data", {fs::path("data") / "folderA" / "test.txt"}, false);
    EXPECT_EQ(pathList.size(), 1);
    EXPECT_STREQ(pathList[0].c_str(), (fs::path("data") / "folderA" / "test.txt").c_str());

}

}
