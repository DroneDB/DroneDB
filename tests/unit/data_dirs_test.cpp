/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The data folders of GDAL and PROJ. GDAL's data files were not in the .deb and nothing told GDAL
// where they are: every raster build of a host that never sourced /etc/profile.d/ddb.sh (Registry
// under dotnet) warned "Cannot find tms_NZTM2000.json (GDAL_DATA is not defined)". PROJ's database
// was looked for in the folder of the executable, which for /usr/bin/ddb or a dotnet host holds
// none. libddb now finds both with its data lookup (getDataPath: DDB_DATA, the folder of the
// executable, /usr/share/ddb, ...) when it registers the process: GDAL_DATA as GDAL's config
// option, the PROJ database as the search path of GDAL's PROJ contexts. What the host set
// (GDAL_DATA, PROJ_DATA, PROJ_LIB) wins, and the log says which folders are in use.
//
// The process registers in main, so the checks run in a child process of this binary whose
// environment this test controls (the child test is disabled: only the parents run it).

#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include <cpl_conv.h>
#include <cpl_string.h>
#include <ogr_srs_api.h>

#include "fs.h"
#include "gtest/gtest.h"
#include "log_forwarding.h"
#include "mio.h"
#include "process_runner.h"
#include "test.h"
#include "testarea.h"
#include "diagnostics.h"

namespace
{
    using namespace ddb;

    // Set by the parent tests: the folders the child must find in use, "" for the GDAL/PROJ
    // folder of libddb's data lookup (the DDB_DATA the parent sets)
    constexpr const char *ExpectGdalEnv = "DDB_TEST_EXPECT_GDAL_DATA";
    constexpr const char *ExpectProjEnv = "DDB_TEST_EXPECT_PROJ_DATA";
    constexpr const char *ChildTest = "DataDirs.DISABLED_childRegisteredProcess";
    constexpr const char *GdalFile = "tms_NZTM2000.json";

    fs::path stagedGdalData() { return io::getExeFolderPath() / "gdal"; }

    std::string projSearchPath()
    {
        const CPLStringList paths(OSRGetPROJSearchPaths());
        return paths.Count() > 0 ? std::string(paths[0]) : std::string();
    }

    std::mutex loggedMutex;
    std::vector<std::string> logged;
    void collect(int, const char *message, const char *, uint64_t, void *)
    {
        const std::lock_guard lock(loggedMutex);
        logged.emplace_back(message);
    }

    bool wasLogged(const std::string &line)
    {
        const std::lock_guard lock(loggedMutex);
        for (const auto &l : logged)
            if (l == line)
                return true;
        return false;
    }

    // Child side, run by the tests below in its own process
    TEST(DataDirs, DISABLED_childRegisteredProcess)
    {
        const char *expectGdal = std::getenv(ExpectGdalEnv);
        const char *expectProj = std::getenv(ExpectProjEnv);
        if (expectGdal == nullptr || expectProj == nullptr)
            GTEST_SKIP() << "Only run by the DataDirs tests of the parent";

        // GDAL: the folder in use, and what GDAL's drivers do with it
        const char *gdalData = CPLGetConfigOption("GDAL_DATA", nullptr);
        ASSERT_NE(gdalData, nullptr) << "GDAL_DATA is not set once the process is registered";
        EXPECT_EQ(std::string(gdalData), expectGdal);
        const char *found = CPLFindFile("gdal", GdalFile);
        ASSERT_NE(found, nullptr) << GdalFile << " not found by GDAL (GDAL_DATA=" << gdalData << ")";
        EXPECT_TRUE(fs::exists(io::fromUtf8(found))) << found;

        // PROJ: the folder in use, and what a transformation needs of proj.db (EPSG codes)
        EXPECT_EQ(projSearchPath(), expectProj);
        OGRSpatialReferenceH wgs84 = OSRNewSpatialReference(nullptr);
        OGRSpatialReferenceH utm = OSRNewSpatialReference(nullptr);
        ASSERT_EQ(OSRImportFromEPSG(wgs84, 4326), OGRERR_NONE);
        ASSERT_EQ(OSRImportFromEPSG(utm, 32633), OGRERR_NONE);
        OSRSetAxisMappingStrategy(wgs84, OAMS_TRADITIONAL_GIS_ORDER);
        OGRCoordinateTransformationH transform = OCTNewCoordinateTransformation(wgs84, utm);
        ASSERT_NE(transform, nullptr) << "no transformation WGS 84 -> UTM 33N: " << CPLGetLastErrorMsg();
        double x = 15.0, y = 45.0, z = 0.0;
        EXPECT_TRUE(OCTTransform(transform, 1, &x, &y, &z));
        EXPECT_NEAR(x, 500000.0, 1.0); // the central meridian of zone 33
        EXPECT_NEAR(y, 4982950.0, 100.0);
        OCTDestroyCoordinateTransformation(transform);
        OSRDestroySpatialReference(wgs84);
        OSRDestroySpatialReference(utm);

        // The log names the folders in use (not the environment variables)
        logfwd::setCallback(&collect, logfwd::kTrace, nullptr);
        diagnostics::printVersions();
        logfwd::unregister();
        EXPECT_TRUE(wasLogged(std::string("GDAL_DATA = ") + expectGdal));
        EXPECT_TRUE(wasLogged(std::string("PROJ search paths = ") + expectProj));
    }

    // A data folder of a "host": proj.db and the gdal folder, copied from the build
    fs::path makeDataFolder(TestArea &area, const std::string &name)
    {
        const fs::path folder = area.getFolder(name);
        fs::copy_file(io::getDataPath("proj.db"), folder / "proj.db", fs::copy_options::overwrite_existing);
        fs::copy(stagedGdalData(), folder / "gdal", fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        return folder;
    }

    // The child runs with GDAL_DATA, PROJ_DATA and PROJ_LIB as given ("" is how it runs with one not
    // set: an empty value is as good as none to libddb) and DDB_DATA as given
    process::Result runChild(const std::string &ddbData, const std::string &gdalData, const std::string &projData,
                             const std::string &expectGdal, const std::string &expectProj)
    {
        const fs::path self = io::getExeFolderPath() / DDB_UNIT_TESTS_NAME;
        process::Options options;
        options.envOverrides = {{"DDB_DATA", ddbData}, {"GDAL_DATA", gdalData},   {"PROJ_DATA", projData},
                                {"PROJ_LIB", ""},      {ExpectGdalEnv, expectGdal}, {ExpectProjEnv, expectProj}};
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
    TEST(DataDirs, gdalDataStagedNextToTheLibrary)
    {
        EXPECT_TRUE(fs::exists(stagedGdalData() / GdalFile)) << stagedGdalData();
        EXPECT_TRUE(fs::exists(stagedGdalData() / "gdalvrt.xsd"));
        // Data files only, not what vcpkg installs around them in share/gdal
        EXPECT_FALSE(fs::exists(stagedGdalData() / "GDALConfig.cmake"));
        EXPECT_FALSE(fs::exists(stagedGdalData() / "vcpkg.spdx.json"));
    }

    // Nothing set by the host: both folders come from the data lookup, here DDB_DATA, which is
    // not the folder of the executable (a host like /usr/bin/ddb has its data elsewhere)
    TEST(DataDirs, registerProcessFindsTheDataWhenTheHostSetsNothing)
    {
        TestArea area(TEST_NAME, true);
        const fs::path dataFolder = makeDataFolder(area, "data");
        const std::string data = io::toUtf8(dataFolder);
        // libddb composes the folder as a path: native separators ("\\" on Windows)
        expectChildPassed(runChild(data, "", "", io::toUtf8(dataFolder / "gdal"), data));
    }

    TEST(DataDirs, registerProcessKeepsTheDataFoldersOfTheHost)
    {
        TestArea area(TEST_NAME, true);
        const std::string ddbData = io::toUtf8(makeDataFolder(area, "ddb-data"));
        const std::string host = io::toUtf8(makeDataFolder(area, "host"));
        expectChildPassed(runChild(ddbData, host + "/gdal", host, host + "/gdal", host));
    }
} // namespace
