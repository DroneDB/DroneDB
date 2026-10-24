/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The test areas of two processes never meet. ctest -j runs every test in a process of its own,
// and two ctest runs (a developer's and a CI job's, two worktrees) may share one temp folder: the
// areas were <temp>/ddb_test_areas/<name>, so two processes running a test of the same name
// removed and refilled each other's files (CAbi.Tiles, GeoGolden.profileOnEveryCrs failed that way).
// Now an area lives in the folder of its process (TestArea::processRoot), removed when the
// process ends.
//
// The check needs a second process: the child test (disabled, run by the parent only) opens the
// area of the same name as the parent's, in a child of this binary.

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "fs.h"
#include "gtest/gtest.h"
#include "mio.h"
#include "process_runner.h"
#include "test.h"
#include "testarea.h"

namespace
{
    using namespace ddb;

    constexpr const char *ParentFileEnv = "DDB_TEST_AREA_PARENT_FILE";
    constexpr const char *AreaName = "TestAreaIsolation-shared-name";
    constexpr const char *AreaMarker = "CHILD_AREA=";
    constexpr const char *ChildTest = "TestAreaIsolation.DISABLED_childOpensTheSameAreaName";

    // Child side, run by the test below in its own process
    TEST(TestAreaIsolation, DISABLED_childOpensTheSameAreaName)
    {
        const char *parentFile = std::getenv(ParentFileEnv);
        if (parentFile == nullptr)
            GTEST_SKIP() << "Only run by the TestAreaIsolation tests of the parent";

        // The parent wrote a file into the area of this name; this process starts from nothing
        TestArea area(AreaName, true);
        EXPECT_TRUE(fs::exists(io::fromUtf8(parentFile))) << "the parent's file is gone: the child removed the parent's area";
        EXPECT_FALSE(fs::exists(area.getPath("parent.txt"))) << "the child sees the file of the parent";
        std::ofstream(area.getPath("child.txt")) << "child";
        std::cout << AreaMarker << io::toUtf8(area.getPath()) << std::endl;
    }

    TEST(TestAreaIsolation, aProcessHasAFolderOfItsOwn)
    {
        TestArea area(TEST_NAME, true);
        EXPECT_EQ(TestArea::processRoot().parent_path().filename(), "ddb_test_areas");
        EXPECT_EQ(area.getFolder().parent_path(), TestArea::processRoot());
        // The same name in the same process is the same folder: tests of a process share its areas
        EXPECT_EQ(TestArea(TEST_NAME).getFolder(), area.getFolder());
    }

    TEST(TestAreaIsolation, anAreaOfTheSameNameInAnotherProcessIsAnotherFolderRemovedWhenItEnds)
    {
        TestArea area(AreaName, true);
        const fs::path parentFile = area.getPath("parent.txt");
        std::ofstream(parentFile) << "parent";

        process::Options options;
        options.envOverrides = {{ParentFileEnv, io::toUtf8(parentFile)}};
        options.maxRuntime = std::chrono::minutes(2);
        const auto result = process::run(io::getExeFolderPath() / DDB_UNIT_TESTS_NAME,
                                         {"--gtest_also_run_disabled_tests", std::string("--gtest_filter=") + ChildTest},
                                         options);
        EXPECT_TRUE(result.success()) << result.failureText(4000);
        EXPECT_NE(result.outputTail.find("[  PASSED  ] 1 test"), std::string::npos) << result.tail(4000);

        const auto marker = result.outputTail.find(AreaMarker);
        ASSERT_NE(marker, std::string::npos) << result.tail(4000);
        const auto start = marker + std::string(AreaMarker).size();
        const fs::path childArea = io::fromUtf8(result.outputTail.substr(start, result.outputTail.find_first_of("\r\n", start) - start));

        EXPECT_NE(childArea, area.getPath());
        EXPECT_FALSE(fs::exists(childArea)) << "the area of the finished child is still there";
        EXPECT_TRUE(fs::exists(parentFile)) << "the child changed the area of the parent";
    }
} // namespace
