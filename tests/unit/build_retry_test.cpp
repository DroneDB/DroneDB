/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>

#include "buildlod_runner.h"
#include "exceptions.h"
#include "gdal_inc.h"
#include "gdal_write_failure.h"
#include "gtest/gtest.h"
#include "helpers/process_test_utils.h"
#include "mio.h"
#include "plyconv.h"
#include "test.h"
#include "testarea.h"
#include "untwine_runner.h"
#include "vector.h"

namespace {
using namespace ddb;

TEST(BuildRetryClassification, VectorTranslateAndClosePropagateStructuredNoSpace) {
    for (const std::string driver : {"MVT", "GPKG"})
        for (const bool onClose : {false, true}) {
            SCOPED_TRACE(driver + (onClose ? " close" : " translate"));
            TestArea area(TEST_NAME + driver + (onClose ? "-close" : "-translate"), true);
            const fs::path input = area.getPath("layer.geojson");
            std::ofstream(input) << R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{"name":"p"},"geometry":{"type":"Point","coordinates":[12.5,41.9]}}]})";
            ddb_test::withVectorWriteFailure(driver.c_str(), onClose, [&] {
                try {
                    buildVector(input.string(), area.getPath("out").string());
                    FAIL() << "vector write should fail";
                } catch (const RetryableBuildException& exception) {
                    EXPECT_EQ(exception.reason(), RetryableBuildReason::NoSpaceOnDevice);
                    if (onClose) EXPECT_NE(std::string(exception.what()).find("output close"), std::string::npos);
                }
            });
            EXPECT_FALSE(fs::exists(area.getPath("out/vec/source.gpkg")));
            EXPECT_FALSE(fs::exists(area.getPath("out/mvt/metadata.json")));
            for (const auto& entry : fs::directory_iterator(area.getPath("out")))
                EXPECT_EQ(entry.path().filename().string().find(".vec-stage-"), std::string::npos);
        }
}

TEST(BuildRetryClassification, BuildLodIdleTimeoutIsStructured) {
    TestArea area(TEST_NAME, true);
    const fs::path input = area.getPath("input.spz");
    std::ofstream(input) << "test input";
    process::Options options;
    options.idleTimeout = std::chrono::milliseconds(150);
    options.envOverrides.emplace_back("DDB_TEST_CHILD_MODE", "idle");
    const auto result = buildlod::runBuildLodResult({input, area.getPath("model.rad"), true, 1, testChildPath()},
                                                  options);
    EXPECT_FALSE(result.succeeded);
    EXPECT_TRUE(result.timedOut);
    EXPECT_THROW(result.throwIfRetryable("primary build: "), RetryableBuildException);
}

TEST(BuildRetryClassification, UntwineIdleTimeoutAbortsPrimaryRatherThanFallback) {
    TestArea area(TEST_NAME, true);
    process::Options options;
    options.idleTimeout = std::chrono::milliseconds(150);
    options.envOverrides.emplace_back("DDB_TEST_CHILD_MODE", "idle");
    const auto result = untwine::runUntwineResult({"input.laz"}, area.getPath("model.copc.laz"),
                                                area.getPath("tmp"), testChildPath(), options);
    EXPECT_FALSE(result.succeeded);
    EXPECT_TRUE(result.timedOut);
    EXPECT_THROW(result.throwIfRetryable("primary build: "), RetryableBuildException);
}

TEST(BuildRetryClassification, ExitZeroWithoutOutputIsPermanent) {
    TestArea area(TEST_NAME, true);
    const fs::path input = area.getPath("input.spz");
    std::ofstream(input) << "test input";
    process::Options options;
    options.envOverrides.emplace_back("DDB_TEST_CHILD_MODE", "no-output");
    const auto result = buildlod::runBuildLodResult({input, area.getPath("model.rad"), true, 1, testChildPath()},
                                                  options);
    EXPECT_FALSE(result.succeeded);
    EXPECT_FALSE(result.timedOut);
    EXPECT_NO_THROW(result.throwIfRetryable("primary build: "));
}

TEST(BuildRetryClassification, OrdinaryToolExitIsPermanentAndReportsAnError) {
    TestArea area(TEST_NAME, true);
    const fs::path input = area.getPath("input.spz");
    std::ofstream(input) << "test input";
    const auto result = buildlod::runBuildLodResult({input, area.getPath("model.rad"), true, 1, testChildPath()});
    EXPECT_FALSE(result.succeeded);
    EXPECT_FALSE(result.timedOut);
    EXPECT_NO_THROW(result.throwIfRetryable("primary build: "));
    EXPECT_FALSE(result.error.empty());
}

TEST(BuildRetryClassification, FilesystemWrappersPreserveCodeWithoutGuessingFromMessage) {
    TestArea area(TEST_NAME, true);
    const fs::path source = area.getPath("missing");
    try {
        io::copy(source, area.getPath("destination"));
        FAIL() << "copy should fail";
    } catch (const FSException& ex) {
        EXPECT_EQ(ex.code(), std::errc::no_such_file_or_directory);
    }
    ToolRunResult result;
    result.error = "disk full and timeout words in an ordinary tool failure";
    EXPECT_NO_THROW(result.throwIfRetryable("primary build: "));
    result.errorCode = std::make_error_code(std::errc::no_space_on_device);
    EXPECT_THROW(result.throwIfRetryable("primary build: "), RetryableBuildException);
}

#ifndef _WIN32
TEST(BuildRetryClassification, ConverterFlushPropagatesRealNoSpaceErrorAndRemovesPartialOutput) {
    if (!fs::exists("/dev/full")) GTEST_SKIP() << "/dev/full unavailable";
    TestArea area(TEST_NAME, true);
    const fs::path input = area.getPath("mesh.ply");
    const fs::path output = area.getPath("mesh.obj");
    std::ofstream(input) << "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\n"
        "property float y\nproperty float z\nelement face 1\nproperty list uchar int vertex_indices\n"
        "end_header\n0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n";
    fs::create_symlink("/dev/full", output);
    try {
        convertPlyToObj(input.string(), output.string());
        FAIL() << "writing to /dev/full should fail";
    } catch (const FSException& ex) {
        EXPECT_EQ(ex.code(), std::errc::no_space_on_device);
    }
    EXPECT_FALSE(fs::exists(output));
    EXPECT_TRUE(fs::exists("/dev/full"));
}
#endif
} // namespace
