/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Only the raster helper (gdal-util) opens the files of users with GDAL. A process that did not
// declare itself the helper (the Registry, ddbcmd, the test binaries) gets a std::logic_error from
// every function of the open policy that would parse such a file, so a code path of the host that
// does it by mistake fails at once instead of exposing the host to the file.

#include <chrono>
#include <stdexcept>
#include <string>

#include "cog_utils.h"
#include "exif.h"
#include "exif_metadata.h"
#include "gdal_open_policy.h"
#include "gtest/gtest-spi.h"
#include "gtest/gtest.h"
#include "mio.h"
#include "process_runner.h"
#include "raster_ops.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"
#include "thermal.h"
#include "user_open_child.h"
#include "utils.h"

namespace
{
    using namespace ddb;

    struct Files
    {
        fs::path raster;
        fs::path vector;
    };

    Files writeFiles(TestArea &ta)
    {
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = 32;
        spec.bands = 1;
        spec.epsg = 32615;
        spec.originX = 576648.71;
        spec.originY = 5188209.785;
        spec.pixelSize = 0.05;
        spec.value = [](int, int x, int y) { return static_cast<double>((x + y) % 250); };
        Files files;
        files.raster = testutils::createGeoRaster(ta.getFolder("dataset") / "ortho.tif", spec);
        files.vector = writeFile(ta.getFolder("dataset") / "layer.geojson",
                                 R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{},)"
                                 R"("geometry":{"type":"Point","coordinates":[9.1,45.2]}}]})");
        return files;
    }

    // The same call, in a process that is the helper
    void expectOpens(const Files &files)
    {
        EXPECT_TRUE(gdalopen::userFileOpensEnabled());
        EXPECT_NE(openUserRaster(io::toUtf8(files.raster)), nullptr);
        EXPECT_NE(openUserVector(io::toUtf8(files.vector)), nullptr);
        EXPECT_NE(identifyUserRaster(io::toUtf8(files.raster)), nullptr);
        EXPECT_TRUE(gdalopen::validateSidecars(io::toUtf8(files.raster)).ok);
        EXPECT_NO_THROW(isOptimizedCog(io::toUtf8(files.raster)));
        EXPECT_EQ(raster::probe(files.raster, raster::ProbeParams()).type, "GeoRaster");
    }

    TEST(UserOpenGate, theHostDoesNotOpenAFileOfAUserAndTheHelperDoes)
    {
        TestArea ta(TEST_NAME, true);
        const Files files = writeFiles(ta);

        // The test binaries are hosts
        EXPECT_THROW(openUserRaster(io::toUtf8(files.raster)), std::logic_error);
        EXPECT_THROW(openUserVector(io::toUtf8(files.vector)), std::logic_error);
        EXPECT_THROW(identifyUserRaster(io::toUtf8(files.raster)), std::logic_error);
        // Also for an extension that is not read, and a file that does not exist: the gate is
        // the first thing that happens
        EXPECT_THROW(openUserRaster("notes.txt"), std::logic_error);
        EXPECT_THROW(openUserVector(io::toUtf8(ta.getPath("missing.geojson"))), std::logic_error);
        // What opens a file by itself
        EXPECT_THROW(gdalopen::validateSidecars(io::toUtf8(files.raster)), std::logic_error);
        EXPECT_THROW(gdalopen::validateVrt(files.raster, files.raster.parent_path()), std::logic_error);
        EXPECT_THROW(isOptimizedCog(io::toUtf8(files.raster)), std::logic_error);
        // The operations of the helper are the helper's: run in the host they refuse as well
        EXPECT_THROW(raster::probe(files.raster, raster::ProbeParams()), std::logic_error);
        EXPECT_FALSE(gdalopen::userFileOpensEnabled());

        try
        {
            openUserRaster(io::toUtf8(files.raster));
        }
        catch (const std::logic_error &e)
        {
            EXPECT_NE(std::string(e.what()).find("openUserRaster"), std::string::npos) << e.what();
            EXPECT_NE(std::string(e.what()).find("gdal-util"), std::string::npos) << e.what();
        }

        // Without the gate the same calls open the files: the process that declares itself the helper
        testutils::runWithUserFileOpens([&] { expectOpens(files); });
        // and the declaration stayed in the child
        EXPECT_FALSE(gdalopen::userFileOpensEnabled());
        EXPECT_THROW(openUserRaster(io::toUtf8(files.raster)), std::logic_error);
    }

    // The test can fail: a body that expects the host's refusal fails in the helper
    TEST(UserOpenGate, aTestInTheChildSeesTheGateOpenAndItsFailuresReachTheTest)
    {
        TestArea ta(TEST_NAME, true);
        const Files files = writeFiles(ta);
        // A failure in the child is a failure of this test: expected here, and counted
        EXPECT_NONFATAL_FAILURE(testutils::runWithUserFileOpens(
                                    [&] { EXPECT_THROW(openUserRaster(io::toUtf8(files.raster)), std::logic_error); }),
                                "In the child that opens user files");
    }

    // The tags of a file are read by the host with metadata-util: the helper gets what they say in
    // its request, and its workers can start no program. In the helper, the code that would read
    // them refuses at once, so a code path of an operation that reaches it fails its tests instead
    // of starting metadata-util (which the sandbox of the worker forbids)
    USER_OPEN_TEST(UserOpenGate, theHelperReadsNoTagsAndStartsNoMetadataReader)
    {
        TestArea ta(TEST_NAME, true);
        const Files files = writeFiles(ta);

        EXPECT_THROW(exif::metadataReader(), std::logic_error);
        EXPECT_THROW(readExifParser(files.raster), std::logic_error);
        EXPECT_THROW(hasThermalTags(io::toUtf8(files.raster)), std::logic_error);
        try
        {
            exif::metadataReader();
        }
        catch (const std::logic_error &e)
        {
            EXPECT_NE(std::string(e.what()).find("metadata-util"), std::string::npos) << e.what();
        }

        // The detection of a thermal raster that is given what the tags say reads none
        EXPECT_FALSE(isThermalImage(io::toUtf8(files.raster), false));
        EXPECT_TRUE(isThermalImage(io::toUtf8(files.raster), true));
    }

    // A body that records nothing: the child of the next test runs it
    USER_OPEN_TEST(UserOpenGate, aBodyThatRecordsNothing)
    {
        EXPECT_TRUE(gdalopen::userFileOpensEnabled());
    }

    // A child that cannot write what its body recorded says why and fails, so that its parent
    // reports the reason rather than a child that ended well without a result
    TEST(UserOpenGate, aChildThatCannotWriteItsResultSaysWhyAndFails)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path result = ta.getPath("not-there") / "result.json";

        process::Options options;
        options.envOverrides = {{testutils::kUserOpenResultEnv, io::toUtf8(result)},
                                {testutils::kUserOpenCallEnv, "0"},
                                {"GTEST_OUTPUT", ""}};
        options.idleTimeout = std::chrono::milliseconds(0);
        const process::Result run =
            process::run(testutils::testBinary(),
                         {"--gtest_filter=UserOpenGate.aBodyThatRecordsNothing", "--gtest_repeat=1",
                          "--gtest_shuffle=0", "--gtest_brief=1"},
                         options);

        EXPECT_TRUE(run.exited) << run.describe();
        EXPECT_NE(run.exitCode, 0) << run.failureText(2000);
        EXPECT_NE(run.outputTail.find("cannot write result " + io::toUtf8(result) + ": "), std::string::npos)
            << run.outputTail;
        EXPECT_FALSE(fs::exists(result));
    }
} // namespace
