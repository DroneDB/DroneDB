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
#include "gtest/gtest.h"
#include "helpers/process_test_utils.h"
#include "mio.h"
#include "plyconv.h"
#include "test.h"
#include "testarea.h"
#include "untwine_runner.h"
#include "vector.h"

namespace ddb_test {
namespace {
// Fault injection lives entirely in the GDAL test driver. Translations still
// copy real source features; the output factory/close supplies a structured OS
// write failure without relying on its human-readable message.
GDALDriver::CreateCallback originalVectorCreate = nullptr;
GDALDriver* failingVectorDriver = nullptr;
bool failVectorClose = false;

class CloseFailingVectorDataset : public GDALDataset {
public:
    explicit CloseFailingVectorDataset(GDALDataset* dataset) : underlying(dataset) {
        SetDescription(dataset->GetDescription());
        eAccess = GA_Update;
    }
    ~CloseFailingVectorDataset() override { if (underlying) GDALClose(underlying); }
#if GDAL_VERSION_NUM >= 3130000
    GDALDriver* GetDriver() const override { return failingVectorDriver; }
#else
    GDALDriver* GetDriver() override { return failingVectorDriver; }
#endif
#if GDAL_VERSION_NUM >= 3120000
    int GetLayerCount() const override { return underlying->GetLayerCount(); }
    const OGRLayer* GetLayer(int index) const override { return underlying->GetLayer(index); }
    int TestCapability(const char* capability) const override { return underlying->TestCapability(capability); }
#else
    int GetLayerCount() override { return underlying->GetLayerCount(); }
    OGRLayer* GetLayer(int index) override { return underlying->GetLayer(index); }
    int TestCapability(const char* capability) override { return underlying->TestCapability(capability); }
#endif
    OGRLayer* GetLayerByName(const char* name) override { return underlying->GetLayerByName(name); }
    OGRErr StartTransaction(int force) override { return underlying->StartTransaction(force); }
    OGRErr CommitTransaction() override { return underlying->CommitTransaction(); }
    OGRErr RollbackTransaction() override { return underlying->RollbackTransaction(); }
#if GDAL_VERSION_NUM >= 3070000
#if GDAL_VERSION_NUM >= 3130000
    CPLErr Close(GDALProgressFunc, void*) override {
#else
    CPLErr Close() override {
#endif
        if (!underlying) return CE_None;
        GDALClose(underlying);
        underlying = nullptr;
        GDALDataset::Close();
        CPLError(CE_Failure, CPLE_FileIO, "test storage rejected final flush");
        errno = ENOSPC;
        return CE_Failure;
    }
#endif
protected:
#if GDAL_VERSION_NUM >= 3120000
    OGRLayer* ICreateLayer(const char* name, const OGRGeomFieldDefn* definition,
                           CSLConstList options) override {
        return underlying->CreateLayer(name, definition, options);
    }
#else
    OGRLayer* ICreateLayer(const char* name, OGRSpatialReference* reference,
                           OGRwkbGeometryType geometry, char** options) override {
        return underlying->CreateLayer(name, reference, geometry, options);
    }
#endif
private:
    GDALDataset* underlying;
};

#if GDAL_VERSION_NUM >= 3130000
using CreateOptions = CSLConstList;
#else
using CreateOptions = char**;
#endif

GDALDataset* failingVectorCreate(const char* name, int width, int height, int bands,
                                 GDALDataType type, CreateOptions options) {
    if (!failVectorClose) {
        CPLError(CE_Failure, CPLE_FileIO, "test storage rejected output creation");
        errno = ENOSPC;
        return nullptr;
    }
    GDALDataset* dataset = originalVectorCreate(name, width, height, bands, type, options);
    return dataset ? new CloseFailingVectorDataset(dataset) : nullptr;
}
} // namespace

void withVectorWriteFailure(const char* driverName, bool onClose, const std::function<void()>& operation) {
    GDALAllRegister();
    auto* driver = GetGDALDriverManager()->GetDriverByName(driverName);
    ASSERT_NE(driver, nullptr);
    ASSERT_NE(driver->pfnCreate, nullptr);
    struct RestoreDriver {
        GDALDriver* driver;
        GDALDriver::CreateCallback original;
        ~RestoreDriver() {
            driver->pfnCreate = original;
            originalVectorCreate = nullptr;
            failingVectorDriver = nullptr;
        }
    } restore{driver, driver->pfnCreate};
    originalVectorCreate = driver->pfnCreate;
    failingVectorDriver = driver;
    failVectorClose = onClose;
    driver->pfnCreate = failingVectorCreate;
    operation();
}
} // namespace ddb_test

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
    const auto result = buildlod::runBuildLodResult(input, area.getPath("model.rad"), true, 1,
                                                  testChildPath(), options);
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
    const auto result = buildlod::runBuildLodResult(input, area.getPath("model.rad"), true, 1,
                                                  testChildPath(), options);
    EXPECT_FALSE(result.succeeded);
    EXPECT_FALSE(result.timedOut);
    EXPECT_NO_THROW(result.throwIfRetryable("primary build: "));
}

TEST(BuildRetryClassification, OrdinaryToolExitIsPermanentAndBoolApiStillReportsError) {
    TestArea area(TEST_NAME, true);
    const fs::path input = area.getPath("input.spz");
    std::ofstream(input) << "test input";
    const auto result = buildlod::runBuildLodResult(input, area.getPath("model.rad"), true, 1,
                                                  testChildPath());
    EXPECT_FALSE(result.succeeded);
    EXPECT_FALSE(result.timedOut);
    EXPECT_NO_THROW(result.throwIfRetryable("primary build: "));
    std::string error;
    EXPECT_FALSE(buildlod::runBuildLod(input, area.getPath("model.rad"), error, true, 1, testChildPath()));
    EXPECT_FALSE(error.empty());
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
