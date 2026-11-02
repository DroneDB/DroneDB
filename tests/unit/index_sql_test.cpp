/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The SQL vocabulary of the index (index_sql.h), the metadata key rule (metakey.h),
// results without out-parameters and const readers.

#include <fstream>

#include "gtest/gtest.h"

#include "database.h"
#include "dbops.h"
#include "entry.h"
#include "exceptions.h"
#include "index_sql.h"
#include "metakey.h"
#include "mio.h"
#include "statement.h"
#include "test.h"
#include "utils.h"

namespace
{
    using namespace ddb;

    // ---------------------------------------------------------------------- const readers

    TEST(IndexConstants, StatementReadersAndEntryWritersWorkOnConstObjects)
    {
        IndexFixture f(TEST_NAME);
        writeFile(f.root / "a.txt", "alpha");
        addToIndex(f.db.get(), {(f.root / "a.txt").string()});

        const auto q = f.db->query("SELECT path, size, mtime, hash FROM entries WHERE path = 'a.txt'");
        ASSERT_TRUE(q->fetch());
        const Statement &readOnly = *q;
        EXPECT_EQ(readOnly.getText(0), "a.txt");
        EXPECT_EQ(readOnly.getInt(1), 5);
        EXPECT_EQ(readOnly.getInt64(1), 5);
        EXPECT_GT(readOnly.getDouble(2), 0.0);
        EXPECT_FALSE(readOnly.getText(3).empty());

        const auto entry = getEntry(f.db.get(), "a.txt");
        ASSERT_TRUE(entry);
        const Entry &constEntry = *entry;
        EXPECT_NE(constEntry.toString().find("Path: a.txt"), std::string::npos);
        EXPECT_FALSE(constEntry.toGeoJSON()) << "a text file has no footprint";
    }

    // ---------------------------------------------------------------- metadata key rule

    TEST(MetaKey, AKeyThatEndsWithSIsAList)
    {
        EXPECT_TRUE(metakey::isList("annotations"));
        EXPECT_TRUE(metakey::isList("s"));
        EXPECT_FALSE(metakey::isList("name"));
        EXPECT_FALSE(metakey::isList(""));
        EXPECT_FALSE(metakey::isList("annotationS")) << "keys are lowercase: S is not the suffix";
    }

    // MetaManager and the aggregate SQL agree on which keys hold arrays
    TEST(MetaKey, TheAggregateSqlAndMetaManagerSplitKeysTheSameWay)
    {
        IndexFixture f(TEST_NAME);
        writeFile(f.root / "a.txt", "alpha");
        addToIndex(f.db.get(), {(f.root / "a.txt").string()});

        auto *meta = f.db->getMetaManager();
        const std::string file = (f.root / "a.txt").string();
        meta->set("name", "\"Alpha\"", file);
        meta->add("notes", "\"one\"", file);
        meta->add("notes", "\"two\"", file);
        EXPECT_THROW(meta->add("note", "\"x\"", file), InvalidArgsException);
        EXPECT_THROW(meta->set("notes", "\"x\"", file), InvalidArgsException);

        const auto q = f.db->query(metaAggregateSql("'a.txt'"));
        ASSERT_TRUE(q->fetch());
        const json aggregate = json::parse(q->getText(0));
        EXPECT_TRUE(aggregate.at("name").is_object());
        ASSERT_TRUE(aggregate.at("notes").is_array());
        EXPECT_EQ(aggregate.at("notes").size(), 2u);

        // The entry readers see the same document
        const auto withMeta = getEntryWithMeta(f.db.get(), "a.txt");
        ASSERT_TRUE(withMeta);
        EXPECT_EQ(withMeta->meta, aggregate);
    }

    TEST(MetaKey, AnEntryWithoutMetadataAggregatesToNull)
    {
        IndexFixture f(TEST_NAME);
        const auto q = f.db->query(metaAggregateSql("'nothing'"));
        ASSERT_TRUE(q->fetch());
        EXPECT_EQ(q->getText(0), "");
    }

    // The dataset's own metadata (empty path) is read through the same aggregate
    TEST(MetaKey, TheDatasetPropertiesCarryTheRootMetadata)
    {
        IndexFixture f(TEST_NAME);
        f.db->getMetaManager()->set("name", "\"Root\"", "");
        f.db->getMetaManager()->add("tags", "\"t\"", "");
        const json properties = f.db->getProperties();
        ASSERT_TRUE(properties.contains("meta"));
        EXPECT_TRUE(properties["meta"].at("name").is_object());
        EXPECT_TRUE(properties["meta"].at("tags").is_array());
    }

    // ----------------------------------------------------------------------- entry columns

    TEST(EntryColumns, EveryLevelReadsWhatItSelectsAndLeavesTheRestEmpty)
    {
        IndexFixture f(TEST_NAME);
        writeFile(f.root / "a.txt", "alpha");
        addToIndex(f.db.get(), {(f.root / "a.txt").string()});
        f.db->getMetaManager()->set("name", "\"A\"", (f.root / "a.txt").string());

        // A generic file has no properties; a row with some shows what the levels read
        insertEntry(f.db.get(), "p.bin", "h", EntryType::Generic);

        const auto read = [&](EntryColumns columns, const std::string &path = "a.txt")
        {
            auto q = f.db->query(selectEntries(columns) + " WHERE e.path = '" + path + "'");
            EXPECT_TRUE(q->fetch());
            Entry e;
            readEntry(*q, columns, e);
            return e;
        };

        const Entry summary = read(EntryColumns::Summary);
        EXPECT_EQ(summary.path, "a.txt");
        EXPECT_EQ(summary.size, 5u);
        EXPECT_EQ(summary.type, EntryType::Generic);
        EXPECT_FALSE(summary.hash.empty());
        EXPECT_TRUE(summary.properties.is_null());
        EXPECT_TRUE(summary.meta.empty());

        EXPECT_TRUE(read(EntryColumns::Summary, "p.bin").properties.is_null());
        const Entry properties = read(EntryColumns::Properties, "p.bin");
        EXPECT_TRUE(properties.properties.is_object());
        EXPECT_TRUE(properties.meta.empty());

        EXPECT_TRUE(read(EntryColumns::Geometries).meta.empty());
        EXPECT_TRUE(read(EntryColumns::Meta).meta.contains("name"));

        // The levels are the same entry
        EXPECT_EQ(json(read(EntryColumns::Meta)).dump(), json(*getEntryWithMeta(f.db.get(), "a.txt")).dump());
    }

    // ------------------------------------------------------------------- IN lists, WHERE

    TEST(IndexSql, InListSqlHasOnePlaceholderPerValue)
    {
        EXPECT_EQ(inListSql(1), "(?)");
        EXPECT_EQ(inListSql(3), "(?,?,?)");
    }

    TEST(IndexSql, ConditionsBindTheirValuesInTheOrderTheyWereAdded)
    {
        IndexFixture f(TEST_NAME);
        insertEntry(f.db.get(), "d/a.txt", "h1", EntryType::Generic);
        insertEntry(f.db.get(), "d/b.jpg", "h2", EntryType::Image);
        insertEntry(f.db.get(), "d/c.laz", "h3", EntryType::PointCloud);
        insertEntry(f.db.get(), "e/d.txt", "h4", EntryType::Generic);

        SqlConditions conditions;
        conditions.add(buildPathPredicate("d", true));
        conditions.addEntryTypes("type", {EntryType::Generic, EntryType::Image});
        conditions.addAtMost("depth", 1);
        conditions.addInStrings("hash", {"h1", "h2", "h4"});

        EXPECT_EQ(conditions.whereClause(),
                  " WHERE path >= ? AND path < ? AND type IN (?,?) AND depth <= ? AND hash IN (?,?,?)");

        auto q = f.db->query("SELECT path FROM entries" + conditions.whereClause() + " ORDER BY path");
        conditions.bind(*q);
        std::vector<std::string> paths;
        while (q->fetch())
            paths.push_back(q->getText(0));
        EXPECT_EQ(paths, (std::vector<std::string>{"d/a.txt", "d/b.jpg"}));
    }

    TEST(IndexSql, NoConditionsIsNoWhere)
    {
        SqlConditions conditions;
        conditions.add(PathPredicate{});
        EXPECT_EQ(conditions.whereClause(), "");
    }

    TEST(IndexSql, TheFolderSqlUsesTheDirectoryTypeNumber)
    {
        EXPECT_NE(insertFolderSql().find("VALUES (?, 1, 'null', ?, 0, ?)"), std::string::npos);
        EXPECT_NE(missingFoldersSql().find("type != 1"), std::string::npos);
        EXPECT_NE(missingFoldersSql().find("type = 1"), std::string::npos);
        EXPECT_EQ(typeCondition("type", "!=", EntryType::DroneDB), "type != 7");
    }

    // A file whose folder has no row of its own: the missing-folders query names the folder, the
    // folder insert creates it as a directory row, and the query is then empty
    TEST(IndexSql, AMissingFolderIsFoundAndCreatedAsADirectoryRow)
    {
        IndexFixture f(TEST_NAME);
        insertEntry(f.db.get(), "m/n/f.txt", "h", EntryType::Generic);

        const auto missing = [&]
        {
            std::vector<std::string> folders;
            auto q = f.db->query(missingFoldersSql());
            while (q->fetch())
                folders.push_back(q->getText(0));
            return folders;
        };
        EXPECT_EQ(missing(), (std::vector<std::string>{"m/n"}));

        auto insert = f.db->query(insertFolderSql());
        insert->bind(1, std::string("m/n"));
        insert->bind(2, 1700000000LL);
        insert->bind(3, 1);
        insert->execute();

        EXPECT_TRUE(missing().empty());
        const auto folder = getEntry(f.db.get(), "m/n");
        ASSERT_TRUE(folder);
        EXPECT_EQ(folder->type, EntryType::Directory);
        EXPECT_EQ(folder->depth, 1);
        EXPECT_EQ(folder->size, 0u);
    }
}
