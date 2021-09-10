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

    auto db = ddb::open(testFolder.string(), false);

    std::vector<std::string> toRemove;
    toRemove.emplace_back((testFolder / "pics.jpg").string());

	removeFromIndex(db.get(), toRemove);

    EXPECT_EQ(countEntries(db.get(), "pics.jpg"), 0);

}

TEST(deleteFromIndex, folderPath) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

    auto db = ddb::open(testFolder.string(), false);

    std::vector<std::string> toRemove;

	// 9
    toRemove.emplace_back((testFolder / "pics").string());

    removeFromIndex(db.get(), toRemove);
    auto cnt = countEntries(db.get());
    EXPECT_EQ(cnt, 15);

}

TEST(deleteFromIndex, subFolderPath) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

    auto db = ddb::open(testFolder.string(), false);

    std::vector<std::string> toRemove;
    // 3
    toRemove.emplace_back((testFolder / "pics" / "pics2").string());

    removeFromIndex(db.get(), toRemove);

    EXPECT_EQ(countEntries(db.get()), 21);

}

TEST(deleteFromIndex, fileExact) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

    auto db = ddb::open(testFolder.string(), false);

    std::vector<std::string> toRemove;
    // 1
    toRemove.emplace_back((testFolder / "1JI_0065.JPG").string());

    removeFromIndex(db.get(), toRemove);

    EXPECT_EQ(countEntries(db.get(), "1JI_0065.JPG"), 0);

}


TEST(deleteFromIndex, fileExactInFolder) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

    auto db = ddb::open(testFolder.string(), false);

    std::vector<std::string> toRemove;
    // 1
    toRemove.emplace_back((testFolder / "pics" / "IMG_20160826_181309.jpg").string());

    removeFromIndex(db.get(), toRemove);

    EXPECT_EQ(countEntries(db.get(), "pics/1JI_0065.JPG"), 0);

}

TEST(deleteFromIndex, fileWildcard) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

    auto db = ddb::open(testFolder.string(), false);

    std::vector<std::string> toRemove;
    // 2
    toRemove.emplace_back((testFolder / "1JI*").string());

    removeFromIndex(db.get(), toRemove);

    EXPECT_EQ(countEntries(db.get()), 22);

}


TEST(deleteFromIndex, fileInFolderWildcard) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

    auto db = ddb::open(testFolder.string(), false);

    std::vector<std::string> toRemove;
    // 5
    toRemove.emplace_back((testFolder / "pics" / "IMG*").string());

    removeFromIndex(db.get(), toRemove);

    EXPECT_EQ(countEntries(db.get()), 19);

	EXPECT_EQ(countEntries(db.get(), "pics/IMG_20160826_181302.jpg"), 0);
    EXPECT_EQ(countEntries(db.get(), "pics/IMG_20160826_181305.jpg"), 0);
    EXPECT_EQ(countEntries(db.get(), "pics/IMG_20160826_181309.jpg"), 0);
    EXPECT_EQ(countEntries(db.get(), "pics/IMG_20160826_181314.jpg"), 0);
    EXPECT_EQ(countEntries(db.get(), "pics/IMG_20160826_181317.jpg"), 0);

}

TEST(deleteFromIndex, fileExactDirtyDot) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

    auto db = ddb::open(testFolder.string(), false);

    std::vector<std::string> toRemove;
    // 1
    toRemove.emplace_back((testFolder / "." / "1JI_0065.JPG").string());

    removeFromIndex(db.get(), toRemove);

    EXPECT_EQ(countEntries(db.get(), "1JI_0065.JPG"), 0);

}

TEST(deleteFromIndex, fileExactDirtyDotDot) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

    auto db = ddb::open(testFolder.string(), false);

    std::vector<std::string> toRemove;
    // 1
    toRemove.emplace_back((testFolder / "pics" / ".." / "1JI_0065.JPG").string());

    removeFromIndex(db.get(), toRemove);

    EXPECT_EQ(countEntries(db.get(), "1JI_0065.JPG"), 0);

}

TEST(listIndex, fileExact) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

    auto db = ddb::open(testFolder.string(), false);

    std::vector<std::string> toList;
    
    toList.emplace_back((testFolder / "1JI_0065.JPG").string());

    std::ostringstream out; 

    listIndex(db.get(), toList, out, "text");

    std::cout << out.str() << std::endl;
    EXPECT_EQ(out.str(), "1JI_0065.JPG\n");
}

TEST(listIndex, allFileWildcard) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

    auto db = ddb::open(testFolder.string(), false);

    std::vector<std::string> toList;
    
    toList.emplace_back((testFolder / "*").string());

    std::ostringstream out; 

    listIndex(db.get(), toList, out, "text");

    std::cout << out.str() << std::endl;
    
    EXPECT_EQ(out.str(), "1JI_0064.JPG\n1JI_0065.JPG\npics\npics.JPG\npics/IMG_20160826_181302.jpg\npics/IMG_20160826_181305.jpg\npics/IMG_20160826_181309.jpg\npics/IMG_20160826_181314.jpg\npics/IMG_20160826_181317.jpg\npics/pics2\npics2\npics2/IMG_20160826_181305.jpg\npics2/IMG_20160826_181309.jpg\npics2/pics\n");

}


TEST(listIndex, rootPath) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

    auto db = ddb::open(testFolder.string(), false);

    std::vector<std::string> toList;

    toList.emplace_back((testFolder / ".").string());
   
    std::ostringstream out; 

    listIndex(db.get(), toList, out, "text");

    std::cout << out.str() << std::endl;
    EXPECT_EQ(out.str(), "1JI_0064.JPG\n1JI_0065.JPG\npics\npics.JPG\npics2\n");

}

TEST(listIndex, rootPath2) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);
    
    std::cout << "Test folder: " << testFolder << std::endl;

    const auto db = ddb::open((testFolder / "pics").string(), true);

    std::vector<std::string> toList;

    toList.emplace_back((testFolder / "pics").string());
   
    std::ostringstream out; 

    listIndex(db.get(), toList, out, "text");

    std::cout << out.str() << std::endl;
    EXPECT_EQ(out.str(), "pics/IMG_20160826_181302.jpg\npics/IMG_20160826_181305.jpg\npics/IMG_20160826_181309.jpg\npics/IMG_20160826_181314.jpg\npics/IMG_20160826_181317.jpg\npics/pics2\n");

}

TEST(listIndex, folder) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);
    
    auto db = ddb::open(testFolder.string(), false);

    std::vector<std::string> toList;

    toList.emplace_back((testFolder / "pics").string());
    
    std::ostringstream out; 

    listIndex(db.get(), toList, out, "text");

    std::cout << out.str() << std::endl;
    EXPECT_EQ(out.str(), "pics/IMG_20160826_181302.jpg\npics/IMG_20160826_181305.jpg\npics/IMG_20160826_181309.jpg\npics/IMG_20160826_181314.jpg\npics/IMG_20160826_181317.jpg\npics/pics2\n");

}

TEST(listIndex, subFolder) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

    auto db = ddb::open(testFolder.string(), false);

    std::vector<std::string> toList;

    toList.emplace_back((testFolder / "pics" / "pics2").string());
    
    std::ostringstream out; 

    listIndex(db.get(), toList, out, "text");

    std::cout << out.str() << std::endl;
    EXPECT_EQ(out.str(), "pics/pics2/IMG_20160826_181305.jpg\npics/pics2/IMG_20160826_181309.jpg\n");

}

TEST(listIndex, fileExactInSubFolderDetails) {

    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/registry/DdbFactoryTest/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);
    
    auto db = ddb::open(testFolder.string(), false);

    std::vector<std::string> toList;

    toList.emplace_back((testFolder / "Sub" / "20200610_144436.jpg").string());
    
    std::ostringstream out; 

    listIndex(db.get(), toList, out, "json");

    auto j = json::parse(out.str());

    std::cout << j.dump(4) << std::endl;

    EXPECT_NE(j, json::value_t::discarded);

    auto el = j[0];

    EXPECT_EQ(el["depth"], 1);
    EXPECT_EQ(el["size"], 8248241);
    EXPECT_EQ(el["type"], 3);
    EXPECT_EQ(el["path"], "Sub/20200610_144436.jpg");

}

TEST(listIndex, fileExactInSubfolder) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);
    
    auto db = ddb::open(testFolder.string(), false);

    std::vector<std::string> toList;

    toList.emplace_back((testFolder / "pics" / "IMG_20160826_181314.jpg").string());
    
    std::ostringstream out; 

    listIndex(db.get(), toList, out, "text");

    std::cout << out.str() << std::endl;
    EXPECT_EQ(out.str(), "pics/IMG_20160826_181314.jpg\n");

}

TEST(listIndex, fileExactInSubfolderWithPathToResolve) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);
    
    auto db = ddb::open(testFolder.string(), false);

    std::vector<std::string> toList;

    toList.emplace_back((testFolder / "pics2" / ".." / "pics" /"IMG_20160826_181314.jpg").string());
    
    std::ostringstream out; 

    listIndex(db.get(), toList, out, "text");

    std::cout << out.str() << std::endl;
    EXPECT_EQ(out.str(), "pics/IMG_20160826_181314.jpg\n");

}

TEST(listIndex, fileExactInSubfolderWithPathToResolve2) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);
    
    auto db = ddb::open(testFolder.string(), false);

    std::vector<std::string> toList;

    toList.emplace_back((testFolder / "pics2" / ".." / "pics" / "." /"IMG_20160826_181314.jpg").string());
    
    std::ostringstream out; 

    listIndex(db.get(), toList, out, "text");

    std::cout << out.str() << std::endl;
    EXPECT_EQ(out.str(), "pics/IMG_20160826_181314.jpg\n");

}

TEST(listIndex, allRecursive) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);
    
    auto db = ddb::open(testFolder.string(), false);

    std::vector<std::string> toList;

    toList.emplace_back((testFolder).string());
    
    std::ostringstream out; 

    listIndex(db.get(), toList, out, "text", true);

    std::cout << out.str() << std::endl;
    EXPECT_EQ(out.str(), "1JI_0064.JPG\n1JI_0065.JPG\npics\npics.JPG\npics/IMG_20160826_181302.jpg\npics/IMG_20160826_181305.jpg\npics/IMG_20160826_181309.jpg\npics/IMG_20160826_181314.jpg\npics/IMG_20160826_181317.jpg\npics/pics2\npics/pics2/IMG_20160826_181305.jpg\npics/pics2/IMG_20160826_181309.jpg\npics2\npics2/IMG_20160826_181305.jpg\npics2/IMG_20160826_181309.jpg\npics2/pics\npics2/pics/IMG_20160826_181302.jpg\npics2/pics/IMG_20160826_181305.jpg\npics2/pics/IMG_20160826_181309.jpg\npics2/pics/IMG_20160826_181314.jpg\npics2/pics/IMG_20160826_181317.jpg\npics2/pics/pics2\npics2/pics/pics2/IMG_20160826_181305.jpg\npics2/pics/pics2/IMG_20160826_181309.jpg\n");

}

TEST(listIndex, folderRecursive) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

    auto db = ddb::open(testFolder.string(), false);

    std::vector<std::string> toList;

    toList.emplace_back((testFolder / "pics").string());
    
    std::ostringstream out; 

    listIndex(db.get(), toList, out, "text", true);

    std::cout << out.str() << std::endl;
    EXPECT_EQ(out.str(), "pics/IMG_20160826_181302.jpg\npics/IMG_20160826_181305.jpg\npics/IMG_20160826_181309.jpg\npics/IMG_20160826_181314.jpg\npics/IMG_20160826_181317.jpg\npics/pics2\npics/pics2/IMG_20160826_181305.jpg\npics/pics2/IMG_20160826_181309.jpg\n");

}

TEST(listIndex, folderRecursiveWithLimit) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

    auto db = ddb::open(testFolder.string(), false);

    std::vector<std::string> toList;

    toList.emplace_back((testFolder / "pics").string());
    
    std::ostringstream out; 

    listIndex(db.get(), toList, out, "text", true, 2);

    std::cout << out.str() << std::endl;
    EXPECT_EQ(out.str(), "pics/IMG_20160826_181302.jpg\npics/IMG_20160826_181305.jpg\npics/IMG_20160826_181309.jpg\npics/IMG_20160826_181314.jpg\npics/IMG_20160826_181317.jpg\npics/pics2\n");

}

TEST(listIndex, wildcardRecursive) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);
    
    auto db = ddb::open(testFolder.string(), false);

    std::vector<std::string> toList;

    toList.emplace_back((testFolder / "pics*").string());
    
    std::ostringstream out; 

    listIndex(db.get(), toList, out, "text", true);

    std::cout << out.str() << std::endl;
    EXPECT_EQ(out.str(), "pics\npics.JPG\npics/IMG_20160826_181302.jpg\npics/IMG_20160826_181305.jpg\npics/IMG_20160826_181309.jpg\npics/IMG_20160826_181314.jpg\npics/IMG_20160826_181317.jpg\npics/pics2\npics/pics2/IMG_20160826_181305.jpg\npics/pics2/IMG_20160826_181309.jpg\npics2\npics2/IMG_20160826_181305.jpg\npics2/IMG_20160826_181309.jpg\npics2/pics\npics2/pics/IMG_20160826_181302.jpg\npics2/pics/IMG_20160826_181305.jpg\npics2/pics/IMG_20160826_181309.jpg\npics2/pics/IMG_20160826_181314.jpg\npics2/pics/IMG_20160826_181317.jpg\npics2/pics/pics2\npics2/pics/pics2/IMG_20160826_181305.jpg\npics2/pics/pics2/IMG_20160826_181309.jpg\n");

}

TEST(listIndex, wildcardRecursiveWithLimit) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);
    
    auto db = ddb::open(testFolder.string(), false);

    std::vector<std::string> toList;

    toList.emplace_back((testFolder / "pics*").string());
    
    std::ostringstream out; 

    listIndex(db.get(), toList, out, "text", true, 2);

    std::cout << out.str() << std::endl;
    EXPECT_EQ(out.str(), "pics\npics.JPG\npics/IMG_20160826_181302.jpg\npics/IMG_20160826_181305.jpg\npics/IMG_20160826_181309.jpg\npics/IMG_20160826_181314.jpg\npics/IMG_20160826_181317.jpg\npics/pics2\npics2\npics2/IMG_20160826_181305.jpg\npics2/IMG_20160826_181309.jpg\npics2/pics\n");

}

TEST(fingerprint, fileHandle) {
    TestArea ta(TEST_NAME);
    fs::path ortho = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/brighton/odm_orthophoto.tif",
                                          "ortho.tif");

    auto fp = ddb::fingerprint(ortho);

    EXPECT_TRUE(fp == ddb::EntryType::GeoRaster);

    // Test if keeps the file open
    // fs::remove(ortho);

}

std::string showList(Database* db, const std::filesystem::path& testFolder) {
    std::vector<std::string> toList;
    toList.emplace_back((testFolder / "*").string());
    std::ostringstream out;
    listIndex(db, toList, out, "text", true);
    const auto str = out.str();
    std::cout << str << std::endl;
    return str;
}

TEST(moveEntry, happyPath) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

    auto db = ddb::open(testFolder.string(), false);

    showList(db.get(), testFolder);

    moveEntry(db.get(), "pics.JPG", "pics2/pics/asd.jpg");//"pacs.jpg");
    
    auto str = showList(db.get(), testFolder);    
    
    EXPECT_EQ(str, "1JI_0064.JPG\n1JI_0065.JPG\npics\npics/IMG_20160826_181302.jpg\npics/IMG_20160826_181305.jpg\npics/IMG_20160826_181309.jpg\npics/IMG_20160826_181314.jpg\npics/IMG_20160826_181317.jpg\npics/pics2\npics/pics2/IMG_20160826_181305.jpg\npics/pics2/IMG_20160826_181309.jpg\npics2\npics2/IMG_20160826_181305.jpg\npics2/IMG_20160826_181309.jpg\npics2/pics\npics2/pics/IMG_20160826_181302.jpg\npics2/pics/IMG_20160826_181305.jpg\npics2/pics/IMG_20160826_181309.jpg\npics2/pics/IMG_20160826_181314.jpg\npics2/pics/IMG_20160826_181317.jpg\npics2/pics/asd.jpg\npics2/pics/pics2\npics2/pics/pics2/IMG_20160826_181305.jpg\npics2/pics/pics2/IMG_20160826_181309.jpg\n");

}

TEST(moveEntry, happyPath2) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

    auto db = ddb::open(testFolder.string(), false);

    showList(db.get(), testFolder);

    moveEntry(db.get(), "pics2", "pics3");//"pacs.jpg");

    auto str = showList(db.get(), testFolder);
    
    EXPECT_EQ(str, "1JI_0064.JPG\n1JI_0065.JPG\npics\npics.JPG\npics/IMG_20160826_181302.jpg\npics/IMG_20160826_181305.jpg\npics/IMG_20160826_181309.jpg\npics/IMG_20160826_181314.jpg\npics/IMG_20160826_181317.jpg\npics/pics2\npics/pics2/IMG_20160826_181305.jpg\npics/pics2/IMG_20160826_181309.jpg\npics3\npics3/IMG_20160826_181305.jpg\npics3/IMG_20160826_181309.jpg\npics3/pics\npics3/pics/IMG_20160826_181302.jpg\npics3/pics/IMG_20160826_181305.jpg\npics3/pics/IMG_20160826_181309.jpg\npics3/pics/IMG_20160826_181314.jpg\npics3/pics/IMG_20160826_181317.jpg\npics3/pics/pics2\npics3/pics/pics2/IMG_20160826_181305.jpg\npics3/pics/pics2/IMG_20160826_181309.jpg\n");

}

TEST(moveEntry, happyPath3) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

    auto db = ddb::open(testFolder.string(), false);

    showList(db.get(), testFolder);

    moveEntry(db.get(), "pics2/pics", "pics3");//"pacs.jpg");

    auto str = showList(db.get(), testFolder);
   
    EXPECT_EQ(str, "1JI_0064.JPG\n1JI_0065.JPG\npics\npics.JPG\npics/IMG_20160826_181302.jpg\npics/IMG_20160826_181305.jpg\npics/IMG_20160826_181309.jpg\npics/IMG_20160826_181314.jpg\npics/IMG_20160826_181317.jpg\npics/pics2\npics/pics2/IMG_20160826_181305.jpg\npics/pics2/IMG_20160826_181309.jpg\npics2\npics2/IMG_20160826_181305.jpg\npics2/IMG_20160826_181309.jpg\npics3\npics3/IMG_20160826_181302.jpg\npics3/IMG_20160826_181305.jpg\npics3/IMG_20160826_181309.jpg\npics3/IMG_20160826_181314.jpg\npics3/IMG_20160826_181317.jpg\npics3/pics2\npics3/pics2/IMG_20160826_181305.jpg\npics3/pics2/IMG_20160826_181309.jpg\n");

}

TEST(moveEntry, conflict) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

    auto db = ddb::open(testFolder.string(), false);

    showList(db.get(), testFolder);

    ASSERT_THROW(moveEntry(db.get(), "pics2/pics", "pics2"), InvalidArgsException);
    
}

TEST(moveEntry, folderOnFile) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

    auto db = ddb::open(testFolder.string(), false);
    
    ASSERT_THROW(moveEntry(db.get(), "pics2", "pics.JPG"), InvalidArgsException);
    ASSERT_THROW(moveEntry(db.get(), "pics2/pics", "pics/pics2/IMG_20160826_181305.jpg"), InvalidArgsException);
    ASSERT_THROW(moveEntry(db.get(), "pics2/pics/pics2/IMG_20160826_181309.jpg", "pics2"), InvalidArgsException);
    ASSERT_THROW(moveEntry(db.get(), "pics/IMG_20160826_181314.jpg", "pics2/pics"), InvalidArgsException);

    auto str = showList(db.get(), testFolder);   

}

TEST(moveEntry, badParameters) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

    auto db = ddb::open(testFolder.string(), false);
    
    ASSERT_THROW(moveEntry(db.get(), "pics2/pics/", "pics2"), InvalidArgsException);
    ASSERT_THROW(moveEntry(db.get(), "pics2/pics", "pics2/"), InvalidArgsException);

    auto str = showList(db.get(), testFolder);   

}

TEST(moveEntry, badParameters2) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

    auto db = ddb::open(testFolder.string(), false);
    
    ASSERT_THROW(moveEntry(db.get(), "pics2/pics/", "pics2/.."), InvalidArgsException);
    ASSERT_THROW(moveEntry(db.get(), "../pics2/pics", "pics2"), InvalidArgsException);

    auto str = showList(db.get(), testFolder);   

}

}
