/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// TagManager::getTag only reads.

#include "gtest/gtest.h"

#include "database.h"
#include "mio.h"
#include "tagmanager.h"
#include "test.h"
#include "utils.h"

namespace
{
    using namespace ddb;

    // Reading the tag of a dataset used to create .ddb/tags.json as a side effect, so a
    // read-only call (ddb tag, DDBGetTag) wrote to a dataset it was only asked to read
    TEST(Tag, GettingTheTagOfADatasetWithoutOneWritesNothing)
    {
        IndexFixture f(TEST_NAME);
        const fs::path tagsFile = f.db->ddbDirectory() / kTagsFile;
        ASSERT_FALSE(fs::exists(tagsFile));

        EXPECT_EQ(TagManager(f.db.get()).getTag(), "");
        EXPECT_FALSE(fs::exists(tagsFile));
    }

    TEST(Tag, ASetTagIsReadBack)
    {
        IndexFixture f(TEST_NAME);
        TagManager manager(f.db.get());
        manager.setTag("localhost:5000/admin/dataset");
        EXPECT_TRUE(fs::exists(f.db->ddbDirectory() / kTagsFile));
        EXPECT_EQ(manager.getTag(), "https://localhost:5000/admin/dataset") << "parseTag adds the scheme";
    }

    TEST(Tag, ATagsFileWithoutATagIsNoTag)
    {
        IndexFixture f(TEST_NAME);
        writeFile(f.db->ddbDirectory() / kTagsFile, "{}");
        EXPECT_EQ(TagManager(f.db.get()).getTag(), "");
    }
}
