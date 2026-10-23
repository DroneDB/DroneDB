/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// GDAL's data files were not in the .deb and nothing told GDAL where they are: every raster build
// of a host that never sourced /etc/profile.d/ddb.sh (Registry under dotnet) warned
// "Cannot find tms_NZTM2000.json (GDAL_DATA is not defined)". libddb now sets GDAL_DATA when it
// registers the process, to the "gdal" folder next to the library or executable (the same places
// as its other data files), unless the host set it.
//
// The process registers in main, so the checks run in a child process of this binary whose
// environment this test controls (the child tests are disabled: only the parent runs them).

#include <cstdlib>
#include <string>

#include <cpl_conv.h>

#include "fs.h"
#include "gtest/gtest.h"
#include "mio.h"
#include "process_runner.h"
#include "test.h"
#include "testarea.h"

namespace
{
    using namespace ddb;

    // Set by the parent test: "default" (GDAL_DATA is not set), or the folder the host sets
    constexpr const char *ChildModeEnv = "DDB_TEST_GDAL_DATA_MODE";
    constexpr const char *ChildTest = "GdalData.DISABLED_childRegisteredProcess";
    constexpr const char *DataFile = "tms_NZTM2000.json";

    fs::path stagedGdalData() { return io::getExeFolderPath() / "gdal"; }

    // Child side, run by the tests below in its own process
    TEST(GdalData, DISABLED_childRegisteredProcess)
    {
        const char *mode = std::getenv(ChildModeEnv);
        if (mode == nullptr)
            GTEST_SKIP() << "Only run by the GdalData tests of the parent";

        const char *dataDir = CPLGetConfigOption("GDAL_DATA", nullptr);
        ASSERT_NE(dataDir, nullptr) << "GDAL_DATA is not set once the process is registered";
        if (std::string(mode) == "default")
        {
            EXPECT_EQ(fs::path(io::fromUtf8(dataDir)), stagedGdalData());
            // What GDAL's drivers do when they open a data file: the warning of every raster build
            const char *found = CPLFindFile("gdal", DataFile);
            ASSERT_NE(found, nullptr) << DataFile << " not found by GDAL (GDAL_DATA=" << dataDir << ")";
            EXPECT_TRUE(fs::exists(io::fromUtf8(found))) << found;
        }
        else
            EXPECT_EQ(std::string(dataDir), mode) << "the GDAL_DATA of the host was replaced";
    }

    // GDAL_DATA with the value the parent gives it; "" is how a child runs with it not set (an
    // empty value is as good as none to GDAL and to libddb)
    process::Result runChild(const std::string &gdalData, const std::string &mode)
    {
        const fs::path self = io::getExeFolderPath() / DDB_UNIT_TESTS_NAME;
        process::Options options;
        options.envOverrides = {{"GDAL_DATA", gdalData}, {ChildModeEnv, mode}};
        options.maxRuntime = std::chrono::minutes(2);
        return process::run(
            self, {"--gtest_also_run_disabled_tests", std::string("--gtest_filter=") + ChildTest}, options);
    }

    void expectChildPassed(const process::Result &r)
    {
        EXPECT_TRUE(r.success()) << r.describe() << "\n" << r.tail(4000);
        EXPECT_NE(r.outputTail.find("[  PASSED  ] 1 test"), std::string::npos) << r.tail(4000);
    }

    // The build stages the data files next to the library: the folder libddb looks for
    TEST(GdalData, stagedNextToTheLibrary)
    {
        EXPECT_TRUE(fs::exists(stagedGdalData() / DataFile)) << stagedGdalData();
        EXPECT_TRUE(fs::exists(stagedGdalData() / "gdalvrt.xsd"));
        // Data files only, not what vcpkg installs around them in share/gdal
        EXPECT_FALSE(fs::exists(stagedGdalData() / "GDALConfig.cmake"));
        EXPECT_FALSE(fs::exists(stagedGdalData() / "vcpkg.spdx.json"));
    }

    TEST(GdalData, registerProcessFindsTheDataWhenGdalDataIsNotSet)
    {
        expectChildPassed(runChild("", "default"));
    }

    TEST(GdalData, registerProcessKeepsTheGdalDataOfTheHost)
    {
        TestArea area(TEST_NAME, true);
        const std::string hostValue = io::toUtf8(area.getFolder());
        expectChildPassed(runChild(hostValue, hostValue));
    }
} // namespace
