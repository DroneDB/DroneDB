/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "dbops.h"
#include "entry.h"
#include "entry_types.h"
#include "exceptions.h"
#include "test.h"
#include "testarea.h"

#include <fstream>

namespace
{

    using namespace ddb;

    // Helper function to get entry from database
    bool getEntryFromDb(Database *db, const std::string &path, Entry &entry)
    {
        auto q = db->query("SELECT path, hash, type, properties, mtime, size, depth FROM entries WHERE path = ?");
        q->bind(1, path);
        if (q->fetch())
        {
            entry.path = q->getText(0);
            entry.hash = q->getText(1);
            entry.type = static_cast<EntryType>(q->getInt(2));
            entry.properties = json::parse(q->getText(3));
            entry.mtime = q->getInt64(4);
            entry.size = q->getInt64(5);
            entry.depth = q->getInt(6);
            return true;
        }
        return false;
    }

    // Helper function to count entries in database
    int countEntries(Database *db, EntryType type = EntryType::Undefined)
    {
        std::string sql = "SELECT COUNT(*) FROM entries";
        if (type != EntryType::Undefined)
        {
            sql += " WHERE type = ?";
        }
        auto q = db->query(sql);
        if (type != EntryType::Undefined)
        {
            q->bind(1, static_cast<int>(type));
        }
        q->fetch();
        return q->getInt(0);
    }

    TEST(typeFromHuman, validTypes)
    {
        EXPECT_EQ(typeFromHuman("image"), EntryType::Image);
        EXPECT_EQ(typeFromHuman("Image"), EntryType::Image);
        EXPECT_EQ(typeFromHuman("IMAGE"), EntryType::Image);
        EXPECT_EQ(typeFromHuman("geoimage"), EntryType::GeoImage);
        EXPECT_EQ(typeFromHuman("GeoImage"), EntryType::GeoImage);
        EXPECT_EQ(typeFromHuman("pointcloud"), EntryType::PointCloud);
        EXPECT_EQ(typeFromHuman("PointCloud"), EntryType::PointCloud);
        EXPECT_EQ(typeFromHuman("georaster"), EntryType::GeoRaster);
        EXPECT_EQ(typeFromHuman("video"), EntryType::Video);
        EXPECT_EQ(typeFromHuman("geovideo"), EntryType::GeoVideo);
        EXPECT_EQ(typeFromHuman("model"), EntryType::Model);
        EXPECT_EQ(typeFromHuman("panorama"), EntryType::Panorama);
        EXPECT_EQ(typeFromHuman("geopanorama"), EntryType::GeoPanorama);
        EXPECT_EQ(typeFromHuman("vector"), EntryType::Vector);
        EXPECT_EQ(typeFromHuman("markdown"), EntryType::Markdown);
        EXPECT_EQ(typeFromHuman("generic"), EntryType::Generic);
        EXPECT_EQ(typeFromHuman("dronedb"), EntryType::DroneDB);
    }

    TEST(typeFromHuman, invalidTypes)
    {
        EXPECT_EQ(typeFromHuman("unknown"), EntryType::Undefined);
        EXPECT_EQ(typeFromHuman(""), EntryType::Undefined);
        EXPECT_EQ(typeFromHuman("xyz123"), EntryType::Undefined);
    }

    TEST(getEntryTypeNames, returnsAllTypes)
    {
        auto names = getEntryTypeNames();

        // Should not be empty
        EXPECT_FALSE(names.empty());

        // Should contain common types (lowercase)
        EXPECT_TRUE(std::find(names.begin(), names.end(), "image") != names.end());
        EXPECT_TRUE(std::find(names.begin(), names.end(), "geoimage") != names.end());
        EXPECT_TRUE(std::find(names.begin(), names.end(), "pointcloud") != names.end());
        EXPECT_TRUE(std::find(names.begin(), names.end(), "georaster") != names.end());

        // Should not contain directory or undefined
        EXPECT_TRUE(std::find(names.begin(), names.end(), "directory") == names.end());
        EXPECT_TRUE(std::find(names.begin(), names.end(), "undefined") == names.end());
    }

    TEST(rescanIndex, basicRescan)
    {
        TestArea ta(TEST_NAME, true);

        // Create test directory and initialize database
        auto testFolder = ta.getFolder();
        ddb::initIndex(testFolder.string());
        auto db = ddb::open(testFolder.string(), true);

        // Create a simple text file
        fs::path textFile = testFolder / "test.txt";
        {
            std::ofstream ofs(textFile);
            ofs << "Hello, World!";
        }

        // Add file to index
        ddb::addToIndex(db.get(), {textFile.string()});

        // Verify file was added
        Entry beforeEntry;
        EXPECT_TRUE(getEntryFromDb(db.get(), "test.txt", beforeEntry));
        EXPECT_EQ(beforeEntry.type, EntryType::Generic);

        // Rescan should succeed
        int rescanCount = 0;
        EXPECT_NO_THROW(
            ddb::rescanIndex(db.get(), {}, true,
                [&rescanCount](const Entry &e, bool success, const std::string &error) {
                    if (success) rescanCount++;
                    return true;
                })
        );

        // Should have rescanned at least the text file
        EXPECT_GE(rescanCount, 1);

        // Entry should still exist after rescan
        Entry afterEntry;
        EXPECT_TRUE(getEntryFromDb(db.get(), "test.txt", afterEntry));
        EXPECT_EQ(afterEntry.type, EntryType::Generic);
    }

    TEST(rescanIndex, rescanWithTypeFilter)
    {
        TestArea ta(TEST_NAME, true);

        // Create test directory and initialize database
        auto testFolder = ta.getFolder();
        ddb::initIndex(testFolder.string());
        auto db = ddb::open(testFolder.string(), true);

        // Create two files: one text and one markdown
        fs::path textFile = testFolder / "test.txt";
        fs::path mdFile = testFolder / "readme.md";
        {
            std::ofstream ofs1(textFile);
            ofs1 << "Hello, World!";
            std::ofstream ofs2(mdFile);
            ofs2 << "# Readme\nThis is a test.";
        }

        // Add files to index
        ddb::addToIndex(db.get(), {textFile.string(), mdFile.string()});

        // Rescan only markdown files
        int rescanCount = 0;
        std::vector<EntryType> typeFilter = {EntryType::Markdown};
        ddb::rescanIndex(db.get(), typeFilter, true,
            [&rescanCount](const Entry &e, bool success, const std::string &error) {
                if (success) rescanCount++;
                return true;
            });

        // Should have rescanned only the markdown file
        EXPECT_EQ(rescanCount, 1);
    }

    TEST(rescanIndex, rescanMissingFile)
    {
        TestArea ta(TEST_NAME, true);

        // Create test directory and initialize database
        auto testFolder = ta.getFolder();
        ddb::initIndex(testFolder.string());
        auto db = ddb::open(testFolder.string(), true);

        // Create and add a file
        fs::path textFile = testFolder / "test.txt";
        {
            std::ofstream ofs(textFile);
            ofs << "Hello, World!";
        }
        ddb::addToIndex(db.get(), {textFile.string()});

        // Remove the file from filesystem (but keep in database)
        fs::remove(textFile);

        // Rescan with stopOnError=true should throw
        EXPECT_THROW(
            ddb::rescanIndex(db.get(), {}, true, nullptr),
            FSException
        );

        // Rescan with stopOnError=false should continue
        int errorCount = 0;
        int successCount = 0;
        EXPECT_NO_THROW(
            ddb::rescanIndex(db.get(), {}, false,
                [&errorCount, &successCount](const Entry &e, bool success, const std::string &error) {
                    if (success) successCount++;
                    else errorCount++;
                    return true;
                })
        );

        // Should have reported the error
        EXPECT_GE(errorCount, 1);
    }

    TEST(rescanIndex, rescanCancellation)
    {
        TestArea ta(TEST_NAME, true);

        // Create test directory and initialize database
        auto testFolder = ta.getFolder();
        ddb::initIndex(testFolder.string());
        auto db = ddb::open(testFolder.string(), true);

        // Create multiple files
        for (int i = 0; i < 5; i++)
        {
            fs::path textFile = testFolder / ("test" + std::to_string(i) + ".txt");
            std::ofstream ofs(textFile);
            ofs << "File " << i;
        }

        // Add all files
        std::vector<std::string> paths;
        for (int i = 0; i < 5; i++)
        {
            paths.push_back((testFolder / ("test" + std::to_string(i) + ".txt")).string());
        }
        ddb::addToIndex(db.get(), paths);

        // Rescan but cancel after first file
        int rescanCount = 0;
        ddb::rescanIndex(db.get(), {}, true,
            [&rescanCount](const Entry &e, bool success, const std::string &error) {
                rescanCount++;
                return false; // Cancel after first
            });

        // Should have processed only one file before cancellation
        EXPECT_EQ(rescanCount, 1);
    }

    TEST(rescanIndex, rescanWithGeoImage)
    {
        TestArea ta(TEST_NAME, true);

        // Download a real geo-referenced image
        fs::path orthoPath = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/master/brighton/odm_orthophoto.tif",
            "ortho.tif");

        // Initialize database and add file
        ddb::initIndex(ta.getFolder().string());
        auto db = ddb::open(ta.getFolder().string(), true);
        ddb::addToIndex(db.get(), {orthoPath.string()});

        // Get entry before rescan
        Entry beforeEntry;
        EXPECT_TRUE(getEntryFromDb(db.get(), "ortho.tif", beforeEntry));
        EXPECT_EQ(beforeEntry.type, EntryType::GeoRaster);

        // Rescan
        int rescanCount = 0;
        ddb::rescanIndex(db.get(), {}, true,
            [&rescanCount](const Entry &e, bool success, const std::string &error) {
                if (success) rescanCount++;
                return true;
            });

        EXPECT_GE(rescanCount, 1);

        // Verify entry still has correct type after rescan
        Entry afterEntry;
        EXPECT_TRUE(getEntryFromDb(db.get(), "ortho.tif", afterEntry));
        EXPECT_EQ(afterEntry.type, EntryType::GeoRaster);

        // Hash should remain the same (file content unchanged)
        EXPECT_EQ(beforeEntry.hash, afterEntry.hash);
    }

    TEST(rescanIndex, invalidPath)
    {
        // Attempt to open a non-existent database should throw
        EXPECT_THROW(
            ddb::open("/nonexistent/path", false),
            FSException
        );
    }

    TEST(rescanIndex, invalidType)
    {
        // typeFromHuman should return Undefined for invalid types
        EXPECT_EQ(typeFromHuman("invalidtype"), EntryType::Undefined);
    }

} // namespace
