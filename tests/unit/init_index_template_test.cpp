/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// D2-7a: initIndex() copies the empty-index template of the user profile
// (<profile>/templates/empty-dbase-<revision>.sqlite), creating it first when the profile has
// none. Processes that start on a fresh profile at the same time used to create it in place
// together (only an in-process mutex guarded it): one copied a half-written file ("Table
// 'entries' not found"), two ran the DDL on the same file ("trigger ... already exists") or
// one hit SQLITE_BUSY. The template is now built in a temporary file of its own and published
// with an atomic rename, so the template path only ever names a complete database.
//
// The racing processes are this binary itself, running the child test below.

#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "database.h"
#include "dbops.h"
#include "mio.h"
#include "process_runner.h"
#include "test.h"
#include "testarea.h"
#include "userprofile.h"

namespace
{
    using namespace ddb;

    // Set by the parent test: the folder the child initializes
    constexpr const char *ChildFolderEnv = "DDB_TEST_INIT_INDEX_FOLDER";
    constexpr const char *ChildTest = "InitIndexTemplate.DISABLED_childInitIndex";

    // Child side, run in separate processes by concurrentProcessesOnFreshProfile (disabled:
    // never part of a normal run).
    TEST(InitIndexTemplate, DISABLED_childInitIndex)
    {
        const char *folder = std::getenv(ChildFolderEnv);
        if (folder == nullptr)
            GTEST_SKIP() << "Only run by InitIndexTemplate.concurrentProcessesOnFreshProfile";
        initIndex(folder);
    }

    void expectValidIndex(const fs::path &root)
    {
        SCOPED_TRACE(root.string());
        const fs::path file = root / DDB_FOLDER / DDB_DATABASE_FILE;
        ASSERT_TRUE(fs::exists(file));

        Database db;
        db.open(file.string());
        EXPECT_TRUE(db.tableExists("entries"));
        EXPECT_TRUE(db.tableExists("entries_meta"));
        EXPECT_TRUE(db.tableExists("passwords"));
        EXPECT_EQ(db.getUserVersion(), DDB_SCHEMA_VERSION);
        const auto check = db.query("PRAGMA integrity_check");
        ASSERT_TRUE(check->fetch());
        EXPECT_EQ(check->getText(0), "ok");
        db.close();
    }

    TEST(InitIndexTemplate, concurrentProcessesOnFreshProfile)
    {
        TestArea area(TEST_NAME, true);
        // DDB_HOME: the profile (and its templates folder) of the children; it has no template
        const fs::path home = area.getFolder("home");
        const fs::path self = io::getExeFolderPath() / DDB_UNIT_TESTS_NAME;

        constexpr int processes = 16;
        std::vector<fs::path> roots;
        std::vector<process::Result> results(processes);
        for (int i = 0; i < processes; i++)
            roots.push_back(area.getFolder("ds" + std::to_string(i)));

        // process::run blocks its thread until the child exits: one thread per child, so they
        // all start together
        std::vector<std::thread> threads;
        for (int i = 0; i < processes; i++)
        {
            threads.emplace_back([&, i] {
                process::Options options;
                options.envOverrides = {{"DDB_HOME", home.string()},
                                        {ChildFolderEnv, roots[i].string()}};
                options.maxRuntime = std::chrono::minutes(2);
                results[i] = process::run(self,
                                          {"--gtest_also_run_disabled_tests",
                                           std::string("--gtest_filter=") + ChildTest},
                                          options);
            });
        }
        for (auto &t : threads)
            t.join();

        for (int i = 0; i < processes; i++)
        {
            EXPECT_TRUE(results[i].success())
                << "child " << i << " " << results[i].describe() << "\n"
                << results[i].tail(4000);
            expectValidIndex(roots[i]);
        }

        // One published template, no temporary file left behind
        const fs::path templates = home / DDB_FOLDER / "templates";
        std::vector<std::string> names;
        for (const auto &e : fs::directory_iterator(templates))
            names.push_back(e.path().filename().string());
        ASSERT_EQ(names.size(), 1u) << ::testing::PrintToString(names);
        EXPECT_EQ(names[0].rfind("empty-dbase-", 0), 0u) << names[0];
        EXPECT_EQ(fs::path(names[0]).extension(), ".sqlite") << names[0];
    }

}
