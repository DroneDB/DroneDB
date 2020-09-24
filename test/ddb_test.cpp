/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "dbops.h"
#include "exceptions.h"
#include "test.h"
#include "testarea.h"

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

TEST(deleteFromIndex, simplePath) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb");
    EXPECT_TRUE(fs::exists(testFolder / ".ddb" / "dbase.sqlite"));

	// TODO: Finish here
}


}
