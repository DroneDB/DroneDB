/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The options of a build (BuildOptions, DDBBuildEx): the cap on the threads of one build is
// checked where it is read, refused when it is not a whole number from 1 to 4096, and followed
// from the C export through the entry handler down to GDAL (raster), PDAL (point cloud without
// Untwine) and the tools a model build runs.

#include <algorithm>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "bm3d_runner.h"
#include "build_budget.h"
#include "dbops.h"
#include "ddb.h"
#include "exceptions.h"
#include "fake_model_tools.h"
#include "fs.h"
#include "gtest/gtest.h"
#include "json_options.h"
#include "logger.h"
#include "mio.h"
#include "obj2tiles_runner.h"
#include "parallel.h"
#include "in_process_raster_helper.h"
#include "raster_helper.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"
#include "user_open_child.h"

namespace {

using namespace ddb;

// ------------------------------------------------------------------------ the resolver

TEST(BuildOptions, withoutACapTheBudgetIsTheThreads) {
    EnvVarGuard threads("DDB_BUILD_THREADS", "5");
    EXPECT_EQ(BuildResourceBudget::fromConfig().threads, 5);
    EXPECT_EQ(BuildResourceBudget::fromConfig(BuildOptions()).threads, 5);
}

TEST(BuildOptions, aCapLowersTheBudgetAndNeverRaisesIt) {
    EnvVarGuard threads("DDB_BUILD_THREADS", "5");
    EXPECT_EQ(BuildResourceBudget::fromConfig(BuildOptions::withThreads(2)).threads, 2);
    EXPECT_EQ(BuildResourceBudget::fromConfig(BuildOptions::withThreads(5)).threads, 5);
    EXPECT_EQ(BuildResourceBudget::fromConfig(BuildOptions::withThreads(4096)).threads, 5);
    EXPECT_EQ(BuildResourceBudget::fromConfig(BuildOptions::withThreads(1)).threads, 1);
    // The rest of the budget does not depend on the cap
    EXPECT_EQ(BuildResourceBudget::fromConfig(BuildOptions::withThreads(2)).cacheBytes,
              BuildResourceBudget::fromConfig().cacheBytes);
}

TEST(BuildOptions, theCapIsAWholeNumberFromOneToTheMaximum) {
    EXPECT_EQ(BuildOptions::withThreads(1).threads, 1);
    EXPECT_EQ(BuildOptions::withThreads(BuildOptions::MaxThreads).threads, BuildOptions::MaxThreads);
    for (const int64_t bad : {int64_t{0}, int64_t{-1}, int64_t{BuildOptions::MaxThreads} + 1, INT64_MIN, INT64_MAX})
        EXPECT_THROW(BuildOptions::withThreads(bad), InvalidArgsException) << bad;
    EXPECT_EQ(BuildOptions::threadsRule(), "threads must be an integer from 1 to 4096");
}

TEST(BuildOptions, anUnknownMemberIsNamedWithTheKnownOnes) {
    try {
        rejectUnknownMembers(json{{"threads", 2}, {"nice", 1}}, {"threads", "priority"}, "build option");
        FAIL() << "an unknown member is refused";
    } catch (const InvalidArgsException &e) {
        EXPECT_STREQ(e.what(), "Unknown build option \"nice\" (known: threads, priority)");
    }
    EXPECT_NO_THROW(rejectUnknownMembers(json{{"threads", 2}}, {"threads", "priority"}, "build option"));
    EXPECT_NO_THROW(rejectUnknownMembers(json::object(), {"threads"}, "build option"));
}

// ------------------------------------------------------------------------ DDBBuildEx

#define ASSERT_DDB_OK(call) ASSERT_EQ((call), DDBERR_NONE) << #call << ": " << DDBGetLastError()

// The text of the last error of a DDBBuildEx call with these options on a dataset that has
// nothing to build: DDBERR_EXCEPTION, and the message names the problem
std::string refusedWith(const std::string &root, const char *options) {
    const DDBErr code = DDBBuildEx(root.c_str(), "a.txt", nullptr, false, false, options);
    EXPECT_EQ(code, DDBERR_EXCEPTION) << options;
    return DDBGetLastError();
}

TEST(DDBBuildEx, refusesOptionsThatAreNotWellFormed) {
    const std::string root = makeIndex(TEST_NAME).string();

    EXPECT_NE(refusedWith(root, "{").find("parse error"), std::string::npos);
    EXPECT_NE(refusedWith(root, "not json").find("parse error"), std::string::npos);
    EXPECT_NE(refusedWith(root, " ").find("parse error"), std::string::npos);
    for (const char *notAnObject : {"[]", "[2]", "2", "\"threads\"", "true", "null"})
        EXPECT_EQ(refusedWith(root, notAnObject), "The build options must be a JSON object") << notAnObject;
}

TEST(DDBBuildEx, refusesAMemberItDoesNotKnow) {
    const std::string root = makeIndex(TEST_NAME).string();
    EXPECT_EQ(refusedWith(root, R"({"thread": 2})"), "Unknown build option \"thread\" (known: threads)");
    EXPECT_EQ(refusedWith(root, R"({"threads": 2, "nice": 5})"), "Unknown build option \"nice\" (known: threads)");
}

TEST(DDBBuildEx, refusesAThreadCountThatIsNotAWholeNumberFromOneTo4096) {
    const std::string root = makeIndex(TEST_NAME).string();
    for (const char *bad : {R"({"threads": 0})", R"({"threads": -1})", R"({"threads": 2.0})",
                            R"({"threads": 2.5})", R"({"threads": true})", R"({"threads": "2"})",
                            R"({"threads": null})", R"({"threads": [2]})", R"({"threads": 4097})",
                            R"({"threads": 99999999999999999999})", R"({"threads": 1e3})"})
        EXPECT_EQ(refusedWith(root, bad), "threads must be an integer from 1 to 4096") << bad;
}

TEST(DDBBuildEx, refusesBadOptionsBeforeItLooksAtTheDataset) {
    // Nothing is built, nothing is opened: the refusal comes first
    const std::string missing = "/nonexistent/dataset";
    EXPECT_EQ(DDBBuildEx(missing.c_str(), nullptr, nullptr, false, false, R"({"threads": 0})"), DDBERR_EXCEPTION);
    EXPECT_NE(std::string(DDBGetLastError()).find("threads must be"), std::string::npos);
}

TEST(DDBBuildEx, acceptsNoOptionsAndACapFromOneTo4096) {
    const std::string root = makeIndex(TEST_NAME).string();
    for (const char *options : {static_cast<const char *>(nullptr), "", "{}", " {} ", R"({"threads": 1})",
                                R"({"threads": 2})", R"({"threads": 4096})"}) {
        // a.txt is not buildable: the call succeeds without building anything
        ASSERT_DDB_OK(DDBBuildEx(root.c_str(), "a.txt", nullptr, false, false, options)) << (options ? options : "NULL");
        ASSERT_DDB_OK(DDBBuildEx(root.c_str(), nullptr, nullptr, false, false, options));
        ASSERT_DDB_OK(DDBBuildEx(root.c_str(), nullptr, nullptr, false, true, options));
    }
}

TEST(DDBBuildEx, keepsTheCodesAndTheArgumentRulesOfDDBBuild) {
    const std::string root = makeIndex(TEST_NAME).string();
    EXPECT_EQ(DDBBuildEx(nullptr, nullptr, nullptr, false, false, "{}"), DDBERR_EXCEPTION);
    EXPECT_EQ(DDBBuildEx(root.c_str(), "../a.txt", nullptr, false, false, "{}"), DDBERR_INVALIDARGS);
    EXPECT_EQ(DDBBuildEx(root.c_str(), "missing.txt", nullptr, false, false, "{}"), DDBERR_EXCEPTION);
    EXPECT_EQ(DDBBuild(root.c_str(), "missing.txt", nullptr, false, false), DDBERR_EXCEPTION);
    EXPECT_EQ(DDBBuild(nullptr, nullptr, nullptr, false, false), DDBERR_EXCEPTION);
}

// ------------------------------------------------------------------------ the cap reaches GDAL and PDAL

// The debug records of libddb while in scope, through the host's log callback
class DebugLog {
public:
    DebugLog() : previous_(plog::get()->getMaxSeverity()) {
        plog::get()->setMaxSeverity(plog::debug);
        DDBSetLogCallback(&DebugLog::collect, DDB_LOG_DEBUG, this);
    }
    ~DebugLog() {
        DDBSetLogCallback(nullptr, DDB_LOG_TRACE, nullptr);
        plog::get()->setMaxSeverity(previous_);
    }
    DebugLog(const DebugLog &) = delete;
    DebugLog &operator=(const DebugLog &) = delete;

    // The records that contain `text`
    std::vector<std::string> matching(const std::string &text) {
        const std::lock_guard lock(mutex_);
        std::vector<std::string> out;
        for (const auto &m : messages_)
            if (m.find(text) != std::string::npos)
                out.push_back(m);
        return out;
    }

private:
    static void collect(int, const char *message, const char *, uint64_t, void *self) {
        auto *log = static_cast<DebugLog *>(self);
        const std::lock_guard lock(log->mutex_);
        log->messages_.emplace_back(message);
    }

    plog::Severity previous_;
    std::mutex mutex_;
    std::vector<std::string> messages_;
};

// The NUM_THREADS of the COG warps of the builds while the log was in scope
std::vector<std::string> cogThreads(DebugLog &log) {
    std::vector<std::string> out;
    for (const auto &m : log.matching("COG warp and compression of "))
        out.push_back(m.substr(m.rfind(' ') + 1));
    return out;
}

// A dataset with one georeferenced raster (ortho.tif), a GeoTIFF that is not a COG yet
struct RasterDataset {
    IndexFixture index;
    explicit RasterDataset(const std::string &name) : index(name, "") {
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = 64;
        spec.epsg = 32632;
        spec.originX = 500000.0;
        spec.originY = 4500000.0;
        spec.value = [](int band, int x, int y) { return static_cast<double>((x * band + y) % 250); };
        testutils::createGeoRaster(index.root / "ortho.tif", spec);
        addToIndex(index.db.get(), {(index.root / "ortho.tif").string()});
    }
    std::string root() const { return index.root.string(); }
};

USER_OPEN_TEST(DDBBuildEx, theCapReachesTheGdalThreadsOfARasterBuildThroughTheEntryHandler) {
    RasterDataset ds(TEST_NAME);
    // The test reads the GDAL options of the warp from the log of the process that builds
    testutils::InProcessRasterHelper helper;
    const raster::ScopedRasterHelper installed(helper);
    const ScopedProcessConfig pool(R"({"cpuPoolSize": 4096})");
    EnvVarGuard budget("DDB_BUILD_THREADS", "4");

    {
        DebugLog log;
        ASSERT_DDB_OK(DDBBuildEx(ds.root().c_str(), "ortho.tif", nullptr, true, false, R"({"threads": 2})"));
        EXPECT_EQ(cogThreads(log), std::vector<std::string>{"NUM_THREADS=2"});
    }
    {
        DebugLog log;
        ASSERT_DDB_OK(DDBBuildEx(ds.root().c_str(), "ortho.tif", nullptr, true, false, nullptr));
        EXPECT_EQ(cogThreads(log), std::vector<std::string>{"NUM_THREADS=4"}) << "without a cap, the budget";
    }
}

// Without Untwine (the PDAL backend is chosen here), writers.copc runs on the threads of the build.
// The test area has a short name: the build converts cloud.xyz to a LAS about 130 characters below
// the root, and the temporary folder of the runner and a test area named after the test (over 70
// characters) took that path to 265, over the 260 of Windows, so PDAL could not create the file.
TEST(DDBBuildEx, theCapReachesTheThreadsOfPdalWritersCopcThroughTheEntryHandler) {
    PointCloudDataset ds("DDBBuildEx.copc");
    const ScopedProcessConfig pool(R"({"cpuPoolSize": 4096})");
    EnvVarGuard budget("DDB_BUILD_THREADS", "4");

    {
        DebugLog log;
        ASSERT_DDB_OK(DDBBuildEx(ds.root().c_str(), "cloud.xyz", nullptr, true, false, R"({"threads": 2})"));
        const auto runs = log.matching("PDAL writers.copc of ");
        ASSERT_EQ(runs.size(), 1u);
        EXPECT_NE(runs[0].find(" on 2 threads"), std::string::npos) << runs[0];
    }
    {
        DebugLog log;
        ASSERT_DDB_OK(DDBBuildEx(ds.root().c_str(), "cloud.xyz", nullptr, true, false, nullptr));
        const auto runs = log.matching("PDAL writers.copc of ");
        ASSERT_EQ(runs.size(), 1u);
        EXPECT_NE(runs[0].find(" on 4 threads"), std::string::npos) << "without a cap, the budget: " << runs[0];
    }
}

// ------------------------------------------------------------------------ the cap reaches the tools

#ifndef _WIN32

using testtools::FakeModelTools;
using testtools::ModelDataset;
using testtools::readText;

TEST(DDBBuildEx, theCapReachesBm3dAndObj2TilesThroughTheEntryHandler) {
    ModelDataset ds(TEST_NAME);
    FakeModelTools tools(ds.index.ta);
    const ScopedProcessConfig pool(R"({"cpuPoolSize": 4096})");
    EnvVarGuard budget("DDB_BUILD_THREADS", "6");

    ASSERT_DDB_OK(DDBBuildEx(ds.root().c_str(), "model.obj", nullptr, true, false, R"({"threads": 2})"));
    EXPECT_EQ(readText(tools.bm3dLog).rfind("--threads 2 ", 0), 0u) << readText(tools.bm3dLog);
    // A cap that does not lower the cores of the host is no cap for Obj2Tiles
    EXPECT_EQ(readText(tools.tilesLog), parallel::hardwareThreads() > 2 ? "count=2\n" : "count=\n");
}

TEST(DDBBuildEx, theCapOfABuildAllAndOfAPendingRetryReachesTheTools) {
    ModelDataset ds(TEST_NAME);
    FakeModelTools tools(ds.index.ta);
    const ScopedProcessConfig pool(R"({"cpuPoolSize": 4096})");
    EnvVarGuard budget("DDB_BUILD_THREADS", "6");

    ASSERT_DDB_OK(DDBBuildEx(ds.root().c_str(), nullptr, nullptr, true, false, R"({"threads": 3})"));
    EXPECT_EQ(readText(tools.bm3dLog).rfind("--threads 3 ", 0), 0u) << readText(tools.bm3dLog);

    // A pending marker makes the retry build the entry again, under the cap of the retry
    const std::string hash = indexedHash(ds.index.db.get(), "model.obj");
    std::ofstream(ds.index.db->buildDirectory() / (hash + ".pending")) << "1\n";
    fs::remove(tools.bm3dLog);
    ASSERT_DDB_OK(DDBBuildEx(ds.root().c_str(), nullptr, nullptr, true, true, R"({"threads": 1})"));
    EXPECT_EQ(readText(tools.bm3dLog).rfind("--threads 1 ", 0), 0u) << readText(tools.bm3dLog);
}

TEST(DDBBuildEx, withoutACapABuildKeepsTheBudgetAsDDBBuildDid) {
    ModelDataset ds(TEST_NAME);
    FakeModelTools tools(ds.index.ta);
    const ScopedProcessConfig pool(R"({"cpuPoolSize": 4096})");
    EnvVarGuard budget("DDB_BUILD_THREADS", "4");

    ASSERT_DDB_OK(DDBBuild(ds.root().c_str(), "model.obj", nullptr, true, false));
    EXPECT_EQ(readText(tools.bm3dLog).rfind("--threads 4 ", 0), 0u) << readText(tools.bm3dLog);

    ASSERT_DDB_OK(DDBBuildEx(ds.root().c_str(), "model.obj", nullptr, true, false, "{}"));
    EXPECT_EQ(readText(tools.bm3dLog).rfind("--threads 4 ", 0), 0u) << readText(tools.bm3dLog);
}

#endif

}  // namespace
