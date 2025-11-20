/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <gtest/gtest.h>
#include "logger.h"
#include "ddb.h"
#include "utils.h"
#include "testarea.h"
#include "testfs.h"
#include <fstream>
#include <iostream>
#include <cstring>

int main(int argc, char **argv)
{

    DDBRegisterProcess(true);


    bool cleanTestData = false;
    for (int i = 1; i < argc; i++)
    {
        if (std::strcmp(argv[i], "--clean-testdata") == 0)
        {
            cleanTestData = true;
            // Remove the argument from argv to prevent Google Test from seeing it
            for (int j = i; j < argc - 1; j++)
            {
                argv[j] = argv[j + 1];
            }
            argc--;
            break;
        }
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0)
        {
            std::cout << "DroneDB Test Runner\n";
            std::cout << "Usage: ddbtest [options] [gtest_options]\n";
            std::cout << "\nDroneDB specific options:\n";
            std::cout << "  --clean-testdata    Clean all test areas and test filesystem caches before running tests\n";
            std::cout << "\nGoogle Test options (run with --help for full list):\n";
            std::cout << "  --gtest_filter=PATTERN    Run only tests matching the pattern\n";
            std::cout << "  --gtest_also_run_disabled_tests    Run disabled tests too\n";
            return 0;
        }
    }

    // Clean test data if requested
    if (cleanTestData)
    {
        std::cout << "Cleaning test data...\n";

        TestArea::clearAll();
        TestFS::clearCache("TestFS");

        std::cout << "Test data cleanup completed.\n";

        // If only cleaning was requested (no other arguments), exit
        if (argc == 1)
            return 0;

    }

    auto time = std::chrono::system_clock::now();

    ::testing::InitGoogleTest(&argc, argv);
    auto res =  RUN_ALL_TESTS();

    auto endTime = std::chrono::system_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - time).count();

    std::cout << "Tests finished in " << toHumanReadableTime(duration) << std::endl;

    return res;

}

