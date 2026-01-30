/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "dbops.h"
#include "exceptions.h"
#include "metamanager.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"
#include "constants.h"

namespace
{

    using namespace ddb;

    TEST(getIndexPathList, includeDirs)
    {

        TestArea ta(TEST_NAME);

        auto dataPath = ta.getFolder("data");
        createTestTree(dataPath);

        auto pathList = ddb::getIndexPathList(dataPath, {(dataPath / "folderA" / "test.txt").string()}, true);
        EXPECT_EQ(pathList.size(), 2);
        EXPECT_TRUE(std::find(pathList.begin(), pathList.end(), dataPath / "folderA" / "test.txt") != pathList.end());
        EXPECT_TRUE(std::find(pathList.begin(), pathList.end(), dataPath / "folderA") != pathList.end());

        pathList = ddb::getIndexPathList(ta.getFolder(), {(dataPath / "folderA" / "test.txt").string(), (dataPath / "folderA" / "folderB" / "test.txt").string()}, true);

        EXPECT_EQ(pathList.size(), 5);
        EXPECT_TRUE(std::find(pathList.begin(), pathList.end(), dataPath / "folderA" / "test.txt") != pathList.end());
        EXPECT_TRUE(std::find(pathList.begin(), pathList.end(), dataPath / "folderA" / "folderB" / "test.txt") != pathList.end());
        EXPECT_TRUE(std::find(pathList.begin(), pathList.end(), dataPath / "folderA") != pathList.end());
        EXPECT_TRUE(std::find(pathList.begin(), pathList.end(), dataPath) != pathList.end());
        EXPECT_TRUE(std::find(pathList.begin(), pathList.end(), dataPath / "folderA" / "folderB") != pathList.end());

        EXPECT_THROW(
            pathList = ddb::getIndexPathList("otherRoot", {
                                                              (dataPath / "folderA" / "test.txt").string(),
                                                          },
                                             true),
            FSException);
    }

    TEST(getIndexPathList, dontIncludeDirs)
    {

        TestArea ta(TEST_NAME);

        auto dataPath = ta.getFolder("data");
        createTestTree(dataPath);

        auto pathList = ddb::getIndexPathList(dataPath, {(dataPath / "folderA" / "test.txt").string()}, false);
        EXPECT_EQ(pathList.size(), 1);
        EXPECT_STREQ(pathList[0].string().c_str(), (dataPath / "folderA" / "test.txt").string().c_str());
    }

    int countEntries(Database *db, const std::string path)
    {
        auto q = db->query("SELECT COUNT(*) FROM entries WHERE Path = ?");
        q->bind(1, path);
        q->fetch();
        const auto cnt = q->getInt(0);
        q->reset();

        return cnt;
    }

    int countEntries(Database *db)
    {

        auto q = db->query("SELECT COUNT(*) FROM entries");
        q->fetch();
        const auto cnt = q->getInt(0);
        q->reset();

        return cnt;
    }

    TEST(deleteFromIndex, simplePath)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

        const auto testFolder = ta.getFolder("test");
        create_directory(testFolder / ".ddb");
        fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

        auto db = ddb::open(testFolder.string(), false);

        std::vector<std::string> toRemove;
        toRemove.emplace_back((testFolder / "pics.jpg").string());

        removeFromIndex(db.get(), toRemove);

        EXPECT_EQ(countEntries(db.get(), "pics.jpg"), 0);
    }

    TEST(deleteFromIndex, folderPath)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

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

    TEST(deleteFromIndex, subFolderPath)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

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

    TEST(deleteFromIndex, fileExact)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

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

    TEST(deleteFromIndex, fileExactInFolder)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

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

    TEST(deleteFromIndex, fileWildcard)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

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

    TEST(deleteFromIndex, fileInFolderWildcard)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

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

    TEST(deleteFromIndex, fileExactDirtyDot)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

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

    TEST(deleteFromIndex, fileExactDirtyDotDot)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

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

    // Helper function to count entries_meta rows
    int countEntriesMeta(Database *db, const std::string &path = "")
    {
        std::string sql = path.empty()
            ? "SELECT COUNT(*) FROM entries_meta"
            : "SELECT COUNT(*) FROM entries_meta WHERE path = ?";
        auto q = db->query(sql);
        if (!path.empty()) q->bind(1, path);
        q->fetch();
        const auto cnt = q->getInt(0);
        q->reset();
        return cnt;
    }

    TEST(deleteFromIndex, deletesAssociatedMetadata)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

        const auto testFolder = ta.getFolder("test");
        create_directory(testFolder / ".ddb");
        fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

        auto db = ddb::open(testFolder.string(), false);
        MetaManager manager(db.get());

        // Add metadata to an entry
        const std::string entryPath = "1JI_0065.JPG";
        manager.add("annotations", R"({"test": "value1"})", entryPath, testFolder.string());
        manager.add("annotations", R"({"test": "value2"})", entryPath, testFolder.string());

        // Verify metadata exists
        EXPECT_EQ(countEntriesMeta(db.get(), entryPath), 2);

        // Remove the entry
        std::vector<std::string> toRemove;
        toRemove.emplace_back((testFolder / entryPath).string());
        removeFromIndex(db.get(), toRemove);

        // Verify entry is removed
        EXPECT_EQ(countEntries(db.get(), entryPath), 0);

        // Verify metadata is also removed (batch delete)
        EXPECT_EQ(countEntriesMeta(db.get(), entryPath), 0);
    }

    TEST(deleteFromIndex, deletesMultipleEntriesMetadata)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

        const auto testFolder = ta.getFolder("test");
        create_directory(testFolder / ".ddb");
        fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

        auto db = ddb::open(testFolder.string(), false);
        MetaManager manager(db.get());

        // Add metadata to multiple entries in the pics folder
        manager.add("annotations", R"({"note": "photo1"})", "pics/IMG_20160826_181302.jpg", testFolder.string());
        manager.add("annotations", R"({"note": "photo2"})", "pics/IMG_20160826_181305.jpg", testFolder.string());
        manager.add("annotations", R"({"note": "photo3"})", "pics/IMG_20160826_181309.jpg", testFolder.string());

        // Verify metadata exists
        EXPECT_EQ(countEntriesMeta(db.get(), "pics/IMG_20160826_181302.jpg"), 1);
        EXPECT_EQ(countEntriesMeta(db.get(), "pics/IMG_20160826_181305.jpg"), 1);
        EXPECT_EQ(countEntriesMeta(db.get(), "pics/IMG_20160826_181309.jpg"), 1);

        // Remove the entire pics folder
        std::vector<std::string> toRemove;
        toRemove.emplace_back((testFolder / "pics").string());
        removeFromIndex(db.get(), toRemove);

        // Verify all metadata for pics folder entries is removed (batch delete covers folder)
        EXPECT_EQ(countEntriesMeta(db.get(), "pics/IMG_20160826_181302.jpg"), 0);
        EXPECT_EQ(countEntriesMeta(db.get(), "pics/IMG_20160826_181305.jpg"), 0);
        EXPECT_EQ(countEntriesMeta(db.get(), "pics/IMG_20160826_181309.jpg"), 0);
    }

    TEST(deleteFromIndex, deletesBuildFolder)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

        const auto testFolder = ta.getFolder("test");
        create_directory(testFolder / ".ddb");
        fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

        auto db = ddb::open(testFolder.string(), false);

        // Get the hash of an entry to create a fake build folder
        auto q = db->query("SELECT hash FROM entries WHERE path = ?");
        q->bind(1, "1JI_0065.JPG");
        EXPECT_TRUE(q->fetch());
        const std::string hash = q->getText(0);
        q->reset();
        EXPECT_FALSE(hash.empty());

        // Create a fake build folder with some content
        const auto buildDir = db->buildDirectory();
        const auto buildFolder = buildDir / hash;
        fs::create_directories(buildFolder);
        fileWriteAllText(buildFolder / "thumb.jpg", "fake thumbnail content");
        fileWriteAllText(buildFolder / "preview.webp", "fake preview content");

        EXPECT_TRUE(fs::exists(buildFolder));
        EXPECT_TRUE(fs::exists(buildFolder / "thumb.jpg"));

        // Remove the entry
        std::vector<std::string> toRemove;
        toRemove.emplace_back((testFolder / "1JI_0065.JPG").string());
        removeFromIndex(db.get(), toRemove);

        // Verify entry is removed
        EXPECT_EQ(countEntries(db.get(), "1JI_0065.JPG"), 0);

        // Verify build folder is removed (parallel deletion)
        EXPECT_FALSE(fs::exists(buildFolder));
    }

    TEST(deleteFromIndex, deletesMultipleBuildFolders)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

        const auto testFolder = ta.getFolder("test");
        create_directory(testFolder / ".ddb");
        fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

        auto db = ddb::open(testFolder.string(), false);
        const auto buildDir = db->buildDirectory();

        // Get hashes of entries in the pics folder and create build folders
        std::vector<std::string> hashes;
        auto q = db->query("SELECT hash FROM entries WHERE path LIKE 'pics/%' AND hash IS NOT NULL AND hash != ''");
        while (q->fetch())
        {
            const std::string hash = q->getText(0);
            if (!hash.empty())
            {
                hashes.push_back(hash);
                const auto buildFolder = buildDir / hash;
                fs::create_directories(buildFolder);
                fileWriteAllText(buildFolder / "thumb.jpg", "fake content");
            }
        }
        q->reset();

        // Verify we have some build folders created
        EXPECT_GT(hashes.size(), 0);
        for (const auto &hash : hashes)
        {
            EXPECT_TRUE(fs::exists(buildDir / hash));
        }

        // Remove the entire pics folder
        std::vector<std::string> toRemove;
        toRemove.emplace_back((testFolder / "pics").string());
        removeFromIndex(db.get(), toRemove);

        // Verify all build folders are removed (parallel deletion)
        for (const auto &hash : hashes)
        {
            EXPECT_FALSE(fs::exists(buildDir / hash));
        }
    }

    TEST(listIndex, fileExact)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

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

    TEST(listIndex, allFileWildcard)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

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

    TEST(listIndex, rootPath)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

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

    TEST(listIndex, rootPath2)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

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

    TEST(listIndex, folder)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

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

    TEST(listIndex, subFolder)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

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

    TEST(listIndex, fileExactInSubFolderDetails)
    {

        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/registry/DdbFactoryTest/dbase.sqlite", DDB_DATABASE_FILE);

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

        auto el = j[0];

        EXPECT_EQ(el["depth"], 1);
        EXPECT_EQ(el["size"], 8248241);
        EXPECT_EQ(el["type"], 3);
        EXPECT_EQ(el["path"], "Sub/20200610_144436.jpg");
    }

    TEST(listIndex, fileExactInSubfolder)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

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

    TEST(listIndex, fileExactInSubfolderWithPathToResolve)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

        const auto testFolder = ta.getFolder("test");
        create_directory(testFolder / ".ddb");
        fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

        auto db = ddb::open(testFolder.string(), false);

        std::vector<std::string> toList;

        toList.emplace_back((testFolder / "pics2" / ".." / "pics" / "IMG_20160826_181314.jpg").string());

        std::ostringstream out;

        listIndex(db.get(), toList, out, "text");

        std::cout << out.str() << std::endl;
        EXPECT_EQ(out.str(), "pics/IMG_20160826_181314.jpg\n");
    }

    TEST(listIndex, fileExactInSubfolderWithPathToResolve2)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

        const auto testFolder = ta.getFolder("test");
        create_directory(testFolder / ".ddb");
        fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

        auto db = ddb::open(testFolder.string(), false);

        std::vector<std::string> toList;

        toList.emplace_back((testFolder / "pics2" / ".." / "pics" / "." / "IMG_20160826_181314.jpg").string());

        std::ostringstream out;

        listIndex(db.get(), toList, out, "text");

        std::cout << out.str() << std::endl;
        EXPECT_EQ(out.str(), "pics/IMG_20160826_181314.jpg\n");
    }

    TEST(listIndex, allRecursive)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

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

    TEST(listIndex, folderRecursive)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

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

    TEST(listIndex, folderRecursiveWithLimit)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

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

    TEST(listIndex, wildcardRecursive)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

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

    TEST(listIndex, wildcardRecursiveWithLimit)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

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

    TEST(fingerprint, fileHandle)
    {
        TestArea ta(TEST_NAME);
        fs::path ortho = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/brighton/odm_orthophoto.tif",
                                              "ortho.tif");

        auto fp = ddb::fingerprint(ortho);

        EXPECT_TRUE(fp == ddb::EntryType::GeoRaster);

        // Test if keeps the file open
        // fs::remove(ortho);
    }

    std::string showList(Database *db, const std::filesystem::path &testFolder)
    {
        std::vector<std::string> toList;
        toList.emplace_back((testFolder / "*").string());
        std::ostringstream out;
        listIndex(db, toList, out, "text", true);
        const auto str = out.str();
        std::cout << str << std::endl;
        return str;
    }

    TEST(moveEntry, happyPath)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

        const auto testFolder = ta.getFolder("test");
        create_directory(testFolder / ".ddb");
        fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

        auto db = ddb::open(testFolder.string(), false);

        showList(db.get(), testFolder);

        moveEntry(db.get(), "pics.JPG", "pics2/pics/asd.jpg"); //"pacs.jpg");

        auto str = showList(db.get(), testFolder);

        EXPECT_EQ(str, "1JI_0064.JPG\n1JI_0065.JPG\npics\npics/IMG_20160826_181302.jpg\npics/IMG_20160826_181305.jpg\npics/IMG_20160826_181309.jpg\npics/IMG_20160826_181314.jpg\npics/IMG_20160826_181317.jpg\npics/pics2\npics/pics2/IMG_20160826_181305.jpg\npics/pics2/IMG_20160826_181309.jpg\npics2\npics2/IMG_20160826_181305.jpg\npics2/IMG_20160826_181309.jpg\npics2/pics\npics2/pics/IMG_20160826_181302.jpg\npics2/pics/IMG_20160826_181305.jpg\npics2/pics/IMG_20160826_181309.jpg\npics2/pics/IMG_20160826_181314.jpg\npics2/pics/IMG_20160826_181317.jpg\npics2/pics/asd.jpg\npics2/pics/pics2\npics2/pics/pics2/IMG_20160826_181305.jpg\npics2/pics/pics2/IMG_20160826_181309.jpg\n");
    }

    TEST(moveEntry, happyPath2)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

        const auto testFolder = ta.getFolder("test");
        create_directory(testFolder / ".ddb");
        fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

        auto db = ddb::open(testFolder.string(), false);

        showList(db.get(), testFolder);

        moveEntry(db.get(), "pics2", "pics3"); //"pacs.jpg");

        auto str = showList(db.get(), testFolder);

        EXPECT_EQ(str, "1JI_0064.JPG\n1JI_0065.JPG\npics\npics.JPG\npics/IMG_20160826_181302.jpg\npics/IMG_20160826_181305.jpg\npics/IMG_20160826_181309.jpg\npics/IMG_20160826_181314.jpg\npics/IMG_20160826_181317.jpg\npics/pics2\npics/pics2/IMG_20160826_181305.jpg\npics/pics2/IMG_20160826_181309.jpg\npics3\npics3/IMG_20160826_181305.jpg\npics3/IMG_20160826_181309.jpg\npics3/pics\npics3/pics/IMG_20160826_181302.jpg\npics3/pics/IMG_20160826_181305.jpg\npics3/pics/IMG_20160826_181309.jpg\npics3/pics/IMG_20160826_181314.jpg\npics3/pics/IMG_20160826_181317.jpg\npics3/pics/pics2\npics3/pics/pics2/IMG_20160826_181305.jpg\npics3/pics/pics2/IMG_20160826_181309.jpg\n");
    }

    TEST(moveEntry, happyPath3)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

        const auto testFolder = ta.getFolder("test");
        create_directory(testFolder / ".ddb");
        fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

        auto db = ddb::open(testFolder.string(), false);

        showList(db.get(), testFolder);

        moveEntry(db.get(), "pics2/pics", "pics3"); //"pacs.jpg");

        auto str = showList(db.get(), testFolder);

        EXPECT_EQ(str, "1JI_0064.JPG\n1JI_0065.JPG\npics\npics.JPG\npics/IMG_20160826_181302.jpg\npics/IMG_20160826_181305.jpg\npics/IMG_20160826_181309.jpg\npics/IMG_20160826_181314.jpg\npics/IMG_20160826_181317.jpg\npics/pics2\npics/pics2/IMG_20160826_181305.jpg\npics/pics2/IMG_20160826_181309.jpg\npics2\npics2/IMG_20160826_181305.jpg\npics2/IMG_20160826_181309.jpg\npics3\npics3/IMG_20160826_181302.jpg\npics3/IMG_20160826_181305.jpg\npics3/IMG_20160826_181309.jpg\npics3/IMG_20160826_181314.jpg\npics3/IMG_20160826_181317.jpg\npics3/pics2\npics3/pics2/IMG_20160826_181305.jpg\npics3/pics2/IMG_20160826_181309.jpg\n");
    }

    TEST(moveEntry, conflict)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

        const auto testFolder = ta.getFolder("test");
        create_directory(testFolder / ".ddb");
        fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

        auto db = ddb::open(testFolder.string(), false);

        showList(db.get(), testFolder);

        ASSERT_THROW(moveEntry(db.get(), "pics2/pics", "pics2"), InvalidArgsException);
    }

    TEST(moveEntry, folderOnFile)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

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

    TEST(moveEntry, badParameters)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

        const auto testFolder = ta.getFolder("test");
        create_directory(testFolder / ".ddb");
        fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

        auto db = ddb::open(testFolder.string(), false);

        ASSERT_THROW(moveEntry(db.get(), "pics2/pics/", "pics2"), InvalidArgsException);
        ASSERT_THROW(moveEntry(db.get(), "pics2/pics", "pics2/"), InvalidArgsException);

        auto str = showList(db.get(), testFolder);
    }

    TEST(moveEntry, badParameters2)
    {
        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", DDB_DATABASE_FILE);

        const auto testFolder = ta.getFolder("test");
        create_directory(testFolder / ".ddb");
        fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

        auto db = ddb::open(testFolder.string(), false);

        ASSERT_THROW(moveEntry(db.get(), "pics2/pics/", "pics2/.."), InvalidArgsException);
        ASSERT_THROW(moveEntry(db.get(), "../pics2/pics", "pics2"), InvalidArgsException);

        auto str = showList(db.get(), testFolder);
    }

}
