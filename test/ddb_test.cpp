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

int countEntries(Database* db, const std::string path)
{
    auto q = db->query("SELECT COUNT(*) FROM entries WHERE Path = ?");
    q->bind(1, path);
    q->fetch();
    const auto cnt = q->getInt(0);
    q->reset();

    return cnt;
}

int countEntries(Database* db)
{
    auto q = db->query("SELECT COUNT(*) FROM entries");
    q->fetch();
    const auto cnt = q->getInt(0);
    q->reset();

    return cnt;
}

TEST(deleteFromIndex, simplePath) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);
    const auto dbPath = testFolder / ".ddb" / "dbase.sqlite";
    EXPECT_TRUE(fs::exists(dbPath));

    Database db;

    db.open(dbPath.string());

    std::vector<std::string> toRemove;
    toRemove.emplace_back((testFolder / "pics.jpg").string());

    removeFromIndex(&db, toRemove);

    EXPECT_EQ(countEntries(&db, "pics.jpg"), 0);
        
    db.close();	
	
}

TEST(deleteFromIndex, folderPath) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);
    const auto dbPath = testFolder / ".ddb" / "dbase.sqlite";
    EXPECT_TRUE(fs::exists(dbPath));

    Database db;

    db.open(dbPath.string());

    std::vector<std::string> toRemove;

	// 9
    toRemove.emplace_back((testFolder / "pics").string());

    removeFromIndex(&db, toRemove);

    EXPECT_EQ(countEntries(&db), 15);

    db.close();

}

TEST(deleteFromIndex, subFolderPath) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);
    const auto dbPath = testFolder / ".ddb" / "dbase.sqlite";
    EXPECT_TRUE(fs::exists(dbPath));

    Database db;

    db.open(dbPath.string());

    std::vector<std::string> toRemove;
    // 3
    toRemove.emplace_back((testFolder / "pics" / "pics2").string());

    removeFromIndex(&db, toRemove);

    EXPECT_EQ(countEntries(&db), 21);

    db.close();

}

TEST(deleteFromIndex, fileExact) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);
    const auto dbPath = testFolder / ".ddb" / "dbase.sqlite";
    EXPECT_TRUE(fs::exists(dbPath));

    Database db;

    db.open(dbPath.string());

    std::vector<std::string> toRemove;
    // 1
    toRemove.emplace_back((testFolder / "1JI_0065.JPG").string());

    removeFromIndex(&db, toRemove);

    EXPECT_EQ(countEntries(&db, "1JI_0065.JPG"), 0);

    db.close();

}


TEST(deleteFromIndex, fileExactInFolder) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);
    const auto dbPath = testFolder / ".ddb" / "dbase.sqlite";
    EXPECT_TRUE(fs::exists(dbPath));

    Database db;

    db.open(dbPath.string());

    std::vector<std::string> toRemove;
    // 1
    toRemove.emplace_back((testFolder / "pics" / "IMG_20160826_181309.jpg").string());

    removeFromIndex(&db, toRemove);

    EXPECT_EQ(countEntries(&db, "pics/1JI_0065.JPG"), 0);

    db.close();

}

TEST(deleteFromIndex, fileWildcard) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);
    const auto dbPath = testFolder / ".ddb" / "dbase.sqlite";
    EXPECT_TRUE(fs::exists(dbPath));

    Database db;

    db.open(dbPath.string());

    std::vector<std::string> toRemove;
    // 2
    toRemove.emplace_back((testFolder / "1JI%").string());

    removeFromIndex(&db, toRemove);

    EXPECT_EQ(countEntries(&db), 22);

    db.close();

}


TEST(deleteFromIndex, fileInFolderWildcard) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);
    const auto dbPath = testFolder / ".ddb" / "dbase.sqlite";
    EXPECT_TRUE(fs::exists(dbPath));

    Database db;

    db.open(dbPath.string());

    std::vector<std::string> toRemove;
    // 5
    toRemove.emplace_back((testFolder / "pics" / "IMG%").string());

    removeFromIndex(&db, toRemove);

    EXPECT_EQ(countEntries(&db), 19);

	EXPECT_EQ(countEntries(&db, "pics/IMG_20160826_181302.jpg"), 0);
    EXPECT_EQ(countEntries(&db, "pics/IMG_20160826_181305.jpg"), 0);
    EXPECT_EQ(countEntries(&db, "pics/IMG_20160826_181309.jpg"), 0);
    EXPECT_EQ(countEntries(&db, "pics/IMG_20160826_181314.jpg"), 0);
    EXPECT_EQ(countEntries(&db, "pics/IMG_20160826_181317.jpg"), 0);

    db.close();

}

TEST(deleteFromIndex, fileExactDirtyDot) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);
    const auto dbPath = testFolder / ".ddb" / "dbase.sqlite";
    EXPECT_TRUE(fs::exists(dbPath));

    Database db;

    db.open(dbPath.string());

    std::vector<std::string> toRemove;
    // 1
    toRemove.emplace_back((testFolder / "." / "1JI_0065.JPG").string());

    removeFromIndex(&db, toRemove);

    EXPECT_EQ(countEntries(&db, "1JI_0065.JPG"), 0);

    db.close();

}

TEST(deleteFromIndex, fileExactDirtyDotDot) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);
    const auto dbPath = testFolder / ".ddb" / "dbase.sqlite";
    EXPECT_TRUE(fs::exists(dbPath));

    Database db;

    db.open(dbPath.string());

    std::vector<std::string> toRemove;
    // 1
    toRemove.emplace_back((testFolder / "pics" / ".." / "1JI_0065.JPG").string());

    removeFromIndex(&db, toRemove);

    EXPECT_EQ(countEntries(&db, "1JI_0065.JPG"), 0);

    db.close();

}

TEST(deleteFromIndex, fileExactInvalidPath) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);
    const auto dbPath = testFolder / ".ddb" / "dbase.sqlite";
    EXPECT_TRUE(fs::exists(dbPath));

    Database db;

    db.open(dbPath.string());

    std::vector<std::string> toRemove;
    // 1
    toRemove.emplace_back((testFolder / "..." / "1JI_0065.JPG").string());

    removeFromIndex(&db, toRemove);

    // Normal, "..." is not a valid folder
    EXPECT_EQ(countEntries(&db, "1JI_0065.JPG"), 1);

    toRemove.clear();

    toRemove.emplace_back((testFolder / "abc" / ".." / "1JI_0065.JPG").string());
    removeFromIndex(&db, toRemove);

    // Should have been removed

    EXPECT_EQ(countEntries(&db, "1JI_0065.JPG"), 0);

    db.close();

}



}
