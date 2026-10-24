/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// SyncManager: the stamp of the last sync with each Registry. A dataset that was never synced
// has none, and what it reports instead is the empty dataset.

#include "dbops.h"
#include "exceptions.h"
#include "gtest/gtest.h"
#include "syncmanager.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace
{
    using namespace ddb;

    constexpr const char *Registry = "https://registry.test";

    TEST(SyncManager, AnEmptyStampIsTheChecksumOfAnIndexWithNothingInIt)
    {
        IndexFixture f(TEST_NAME);
        const auto empty = Database::emptyStamp();
        EXPECT_EQ(empty, f.db->getStamp());
        EXPECT_EQ(empty["checksum"].get<std::string>(), f.db->getStampChecksum());
        EXPECT_TRUE(empty["entries"].empty());
        EXPECT_TRUE(empty["meta"].empty());
    }

    // The baseline of a first pull and the checksum of a first push
    TEST(SyncManager, NeverSyncedIsTheEmptyDatasetEvenWhenTheDatasetHasFiles)
    {
        IndexFixture f(TEST_NAME);
        writeFile(f.root / "a.txt", "alpha");
        addToIndex(f.db.get(), {(f.root / "a.txt").string()});
        ASSERT_NE(f.db->getStampChecksum(), Database::emptyStamp()["checksum"].get<std::string>());

        SyncManager sync(f.db.get());
        EXPECT_EQ(sync.getLastStamp(Registry), Database::emptyStamp());
    }

    TEST(SyncManager, ReadingTheLastStampWritesNothing)
    {
        IndexFixture f(TEST_NAME);
        SyncManager sync(f.db.get());
        sync.getLastStamp(Registry);
        EXPECT_FALSE(fs::exists(f.db->ddbDirectory() / SYNCFILE));
    }

    TEST(SyncManager, EveryRegistryHasItsOwnLastStamp)
    {
        IndexFixture f(TEST_NAME);
        writeFile(f.root / "a.txt", "alpha");
        addToIndex(f.db.get(), {(f.root / "a.txt").string()});
        SyncManager sync(f.db.get());

        sync.setLastStamp(Registry, f.db.get());

        EXPECT_EQ(sync.getLastStamp(Registry), f.db->getStamp());
        EXPECT_EQ(sync.getLastStamp("https://other.test"), Database::emptyStamp());

        sync.setLastStamp(Registry, json{{"checksum", "x"}});
        EXPECT_EQ(sync.getLastStamp(Registry)["checksum"], "x");
    }

    TEST(SyncManager, ARegistryIsRequired)
    {
        IndexFixture f(TEST_NAME);
        SyncManager sync(f.db.get());
        EXPECT_THROW(sync.getLastStamp(""), InvalidArgsException);
        EXPECT_THROW(sync.setLastStamp("", json::object()), InvalidArgsException);
    }
}
