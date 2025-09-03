/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <gtest/gtest.h>
#include "logger.h"
#include "ddb.h"
#include "utils.h"
#include <fstream>

int main(int argc, char **argv)
{

    DDBRegisterProcess(true);

    // TODO: ability to clean previous TestAreas

    auto time = std::chrono::system_clock::now();

    ::testing::InitGoogleTest(&argc, argv);
    auto res =  RUN_ALL_TESTS();

    auto endTime = std::chrono::system_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - time).count();

    std::cout << "Tests finished in " << toHumanReadableTime(duration) << std::endl;

    return res;

}

