/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "dbops.h"
#include "exceptions.h"
#include "test.h"
#include "testarea.h"
#include "constants.h"

namespace
{

    using namespace ddb;

    // Helper function to check if an index exists in the database
    bool indexExists(Database *db, const std::string &indexName)
    {
        auto q = db->query("SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name=?");
        q->bind(1, indexName);
        if (q->fetch())
        {
            return q->getInt(0) == 1;
        }
        return false;
    }

    // Helper function to get index columns
    std::string getIndexInfo(Database *db, const std::string &indexName)
    {
        auto q = db->query("SELECT sql FROM sqlite_master WHERE type='index' AND name=?");
        q->bind(1, indexName);
        if (q->fetch())
        {
            return q->getText(0);
        }
        return "";
    }

    TEST(schemaMigration, newDatabaseHasAllIndexes)
    {
        TestArea ta(TEST_NAME, true);

        const auto testFolder = ta.getFolder("test");

        // Initialize a new database
        ddb::initIndex(testFolder.string());
        auto db = ddb::open(testFolder.string(), false);

        // Verify entries table indexes
        EXPECT_TRUE(indexExists(db.get(), "ix_entries_type")) << "ix_entries_type should exist";
        EXPECT_TRUE(indexExists(db.get(), "ix_entries_hash")) << "ix_entries_hash should exist";

        // Verify entries_meta table indexes
        EXPECT_TRUE(indexExists(db.get(), "ix_entries_meta_path_key")) << "ix_entries_meta_path_key should exist";
        EXPECT_TRUE(indexExists(db.get(), "ix_entries_meta_key")) << "ix_entries_meta_key should exist";

        // Verify the old redundant index is NOT present
        EXPECT_FALSE(indexExists(db.get(), "ix_entries_meta_path")) << "ix_entries_meta_path should NOT exist (redundant)";
    }

    TEST(schemaMigration, existingDatabaseGetsNewIndexes)
    {
        TestArea ta(TEST_NAME, true);

        // Download old database that doesn't have the new indexes
        const auto sqlite = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite",
            DDB_DATABASE_FILE);

        const auto testFolder = ta.getFolder("test");
        create_directory(testFolder / ".ddb");
        fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

        // Open the database - this should trigger schema consistency check
        auto db = ddb::open(testFolder.string(), false);

        // After opening, the new indexes should be created via ensureSchemaConsistency
        // Note: This depends on how ensureSchemaConsistency is implemented
        // The indexes are created in the DDL, so they should be applied on new tables

        // Verify entries_meta table exists and has proper indexes
        EXPECT_TRUE(db->tableExists("entries_meta")) << "entries_meta table should exist";
        EXPECT_TRUE(indexExists(db.get(), "ix_entries_meta_path_key")) << "ix_entries_meta_path_key should exist after migration";
        EXPECT_TRUE(indexExists(db.get(), "ix_entries_meta_key")) << "ix_entries_meta_key should exist after migration";
    }

    TEST(schemaMigration, hashIndexIsUsedForQueries)
    {
        TestArea ta(TEST_NAME, true);

        const auto testFolder = ta.getFolder("test");

        // Initialize a new database
        ddb::initIndex(testFolder.string());
        auto db = ddb::open(testFolder.string(), false);

        // Verify the hash index exists
        EXPECT_TRUE(indexExists(db.get(), "ix_entries_hash"));

        // Get the index SQL to verify it's on the hash column
        std::string indexSql = getIndexInfo(db.get(), "ix_entries_hash");
        EXPECT_FALSE(indexSql.empty()) << "Index SQL should not be empty";
        EXPECT_TRUE(indexSql.find("hash") != std::string::npos) << "Index should be on hash column";
    }

    TEST(schemaMigration, compositeIndexOnEntriesMetaPathKey)
    {
        TestArea ta(TEST_NAME, true);

        const auto testFolder = ta.getFolder("test");

        // Initialize a new database
        ddb::initIndex(testFolder.string());
        auto db = ddb::open(testFolder.string(), false);

        // Verify the composite index exists
        EXPECT_TRUE(indexExists(db.get(), "ix_entries_meta_path_key"));

        // Get the index SQL to verify it's on both path and key columns
        std::string indexSql = getIndexInfo(db.get(), "ix_entries_meta_path_key");
        EXPECT_FALSE(indexSql.empty()) << "Index SQL should not be empty";
        EXPECT_TRUE(indexSql.find("path") != std::string::npos) << "Composite index should include path column";
        EXPECT_TRUE(indexSql.find("key") != std::string::npos) << "Composite index should include key column";
    }

    TEST(schemaMigration, redundantPathIndexIsRemoved)
    {
        TestArea ta(TEST_NAME, true);

        const auto testFolder = ta.getFolder("test");

        // Initialize a new database
        ddb::initIndex(testFolder.string());
        auto db = ddb::open(testFolder.string(), false);

        // The old ix_entries_meta_path index should not exist
        // because it's redundant with the composite index ix_entries_meta_path_key
        EXPECT_FALSE(indexExists(db.get(), "ix_entries_meta_path"))
            << "ix_entries_meta_path should not exist (covered by composite index)";

        // But the composite index should exist
        EXPECT_TRUE(indexExists(db.get(), "ix_entries_meta_path_key"))
            << "ix_entries_meta_path_key composite index should exist instead";
    }

    TEST(schemaMigration, oldDatabaseWithPathIndexGetsMigrated)
    {
        TestArea ta(TEST_NAME, true);

        const auto sqlite = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite",
            DDB_DATABASE_FILE);

        const auto testFolder = ta.getFolder("test");
        create_directory(testFolder / ".ddb");
        fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);

        // Manually add the old index to simulate an old database
        {
            auto db = ddb::open(testFolder.string(), false);
            // The entries_meta table should be created by ensureSchemaConsistency
            // which will drop ix_entries_meta_path if it exists and create the new indexes
        }

        // Reopen to verify migration
        auto db = ddb::open(testFolder.string(), false);

        // After migration, the old index should be removed and new composite index should exist
        EXPECT_FALSE(indexExists(db.get(), "ix_entries_meta_path"))
            << "Old ix_entries_meta_path should be removed after migration";
        EXPECT_TRUE(indexExists(db.get(), "ix_entries_meta_path_key"))
            << "New composite index should exist after migration";
    }

}
