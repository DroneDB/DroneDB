/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <database.h>
#include <passwordmanager.h>


#include "gtest/gtest.h"
#include "entry.h"
#include "test.h"
#include "testarea.h"

namespace {

	using namespace ddb;

	TEST(passwordManager, append) {

        TestArea ta(TEST_NAME);

        const auto sqlite = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/dbase.sqlite", "dbase.sqlite");

        const auto testFolder = ta.getFolder("test");
        create_directory(testFolder / ".ddb");
        fs::copy(sqlite.string(), testFolder / ".ddb", fs::copy_options::overwrite_existing);
        const auto dbPath = testFolder / ".ddb" / "dbase.sqlite";
        EXPECT_TRUE(fs::exists(dbPath));

        Database db;
        
        db.open(dbPath.string());
        db.createTables();
		
        PasswordManager manager(&db);

        manager.append("testpassword");

        EXPECT_TRUE(manager.verify("testpassword"));
        EXPECT_FALSE(manager.verify("wrongpassword"));

	}

}
