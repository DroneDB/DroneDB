/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The gdalCacheMb setting of DDBConfigure sets GDAL's process-wide block cache.
//  - set with DDBConfigure it is the size of the cache (raising it too), at once;
//  - put back to null it restores the cache GDAL had, then bounds it by the environment variable
//    or the default, as DDBRegisterProcess does;
//  - a build in flight (ScopedGdalCacheMax) keeps its own size until it ends, then the cache takes
//    the configured one;
//  - an invalid value changes nothing.

#include <cstdint>
#include <string>

#include "build_budget.h"
#include "ddb.h"
#include "gdal_inc.h"
#include "gtest/gtest.h"
#include "json.h"
#include "test.h"
#include "utils.h"

namespace {

using namespace ddb;

constexpr int64_t MiB = 1024 * 1024;

#define ASSERT_DDB_OK(call) ASSERT_EQ((call), DDBERR_NONE) << #call << ": " << DDBGetLastError()

json effective() {
    char* out = nullptr;
    EXPECT_EQ(DDBGetEffectiveConfig(&out), DDBERR_NONE) << DDBGetLastError();
    const json config = json::parse(out != nullptr ? out : "{}");
    DDBFree(out);
    return config;
}

// Puts GDAL's cache and the setting back, whatever the test did
class GdalCacheFixture : public ::testing::Test {
protected:
    void SetUp() override {
        before_ = GDALGetCacheMax64();
        ASSERT_DDB_OK(DDBConfigure(R"({"gdalCacheMb": null})"));
        // The cache the process runs with: bounded by DDBRegisterProcess or by this reset
        start_ = GDALGetCacheMax64();
    }
    void TearDown() override {
        DDBConfigure(R"({"gdalCacheMb": null})");
        GDALSetCacheMax64(before_);
    }

    int64_t before_ = 0;
    int64_t start_ = 0;
};

TEST_F(GdalCacheFixture, configuredSizeIsTheCacheSizeRaisingAndLowering) {
    ASSERT_DDB_OK(DDBConfigure(R"({"gdalCacheMb": 96})"));
    EXPECT_EQ(GDALGetCacheMax64(), 96 * MiB);
    EXPECT_EQ(effective()["settings"]["gdalCacheMb"], 96);
    EXPECT_EQ(effective()["sources"]["gdalCacheMb"], "configure");

    // Above what GDAL had: the host's choice wins, the cache is raised
    ASSERT_DDB_OK(DDBConfigure(R"({"gdalCacheMb": 3000})"));
    EXPECT_EQ(GDALGetCacheMax64(), 3000 * MiB);
}

TEST_F(GdalCacheFixture, nullRestoresTheCacheGdalHad) {
    ASSERT_DDB_OK(DDBConfigure(R"({"gdalCacheMb": 96})"));
    ASSERT_DDB_OK(
        DDBConfigure(R"({"gdalCacheMb": 3000})"));  // the second change keeps the first "before"
    ASSERT_DDB_OK(DDBConfigure(R"({"gdalCacheMb": null})"));
    EXPECT_EQ(GDALGetCacheMax64(), start_);
    EXPECT_EQ(effective()["sources"]["gdalCacheMb"], "default");
    EXPECT_EQ(effective()["settings"]["gdalCacheMb"], 1024);
}

// From the environment variable it is an upper bound: it lowers the cache and never raises it
TEST_F(GdalCacheFixture, environmentVariableIsAnUpperBound) {
    GDALSetCacheMax64(1000 * MiB);
    {
        const EnvVarGuard env("DDB_GDAL_CACHE_MB", "64");
        ASSERT_DDB_OK(DDBConfigure(R"({"gdalCacheMb": null})"));
        EXPECT_EQ(effective()["sources"]["gdalCacheMb"], "environment");
        EXPECT_EQ(effective()["settings"]["gdalCacheMb"], 64);
        EXPECT_EQ(GDALGetCacheMax64(), 64 * MiB);
    }
    {
        const EnvVarGuard env("DDB_GDAL_CACHE_MB", "4096");
        ASSERT_DDB_OK(DDBConfigure(R"({"gdalCacheMb": null})"));
        EXPECT_EQ(GDALGetCacheMax64(), 64 * MiB) << "a bound never raises the cache";
    }
    {
        // Not a valid size: ignored, the default bound applies
        const EnvVarGuard env("DDB_GDAL_CACHE_MB", "8");
        EXPECT_EQ(effective()["sources"]["gdalCacheMb"], "default");
    }
}

// A GDAL_CACHEMAX set explicitly beats the bound, not DDBConfigure
TEST_F(GdalCacheFixture, explicitGdalCacheMaxBeatsTheBoundButNotTheHost) {
    const struct Option {
        Option() { CPLSetConfigOption("GDAL_CACHEMAX", "2048"); }
        ~Option() { CPLSetConfigOption("GDAL_CACHEMAX", nullptr); }
    } option;
    GDALSetCacheMax64(2048 * MiB);
    const EnvVarGuard env("DDB_GDAL_CACHE_MB", "64");
    ASSERT_DDB_OK(DDBConfigure(R"({"gdalCacheMb": null})"));
    EXPECT_EQ(GDALGetCacheMax64(), 2048 * MiB);

    ASSERT_DDB_OK(DDBConfigure(R"({"gdalCacheMb": 128})"));
    EXPECT_EQ(GDALGetCacheMax64(), 128 * MiB);
}

// A build in flight asked for its own size: the configured one comes when the last build ends
TEST_F(GdalCacheFixture, buildInFlightKeepsItsCacheUntilItEnds) {
    {
        const ScopedGdalCacheMax build(512 * MiB);
        EXPECT_EQ(GDALGetCacheMax64(), 512 * MiB);
        ASSERT_DDB_OK(DDBConfigure(R"({"gdalCacheMb": 96})"));
        EXPECT_EQ(GDALGetCacheMax64(), 512 * MiB);
    }
    EXPECT_EQ(GDALGetCacheMax64(), 96 * MiB);

    // The setting put back while a build runs restores the cache when it ends
    {
        const ScopedGdalCacheMax build(512 * MiB);
        ASSERT_DDB_OK(DDBConfigure(R"({"gdalCacheMb": null})"));
        EXPECT_EQ(GDALGetCacheMax64(), 512 * MiB);
    }
    EXPECT_EQ(GDALGetCacheMax64(), start_);
}

TEST_F(GdalCacheFixture, invalidValueChangesNothing) {
    ASSERT_DDB_OK(DDBConfigure(R"({"gdalCacheMb": 96})"));
    for (const char* invalid : {R"({"gdalCacheMb": 15})",
                                R"({"gdalCacheMb": 1048577})",
                                R"({"gdalCacheMb": 0})",
                                R"({"gdalCacheMb": -1})",
                                R"({"gdalCacheMb": 64.5})",
                                R"({"gdalCacheMb": "64"})",
                                R"({"gdalCacheMb": 200, "ioThreads": 0})"}) {
        // The valid member of the last one is not applied either: every member is checked first
        EXPECT_EQ(DDBConfigure(invalid), DDBERR_INVALIDARGS) << invalid;
        EXPECT_STRNE(DDBGetLastError(), "");
        EXPECT_EQ(GDALGetCacheMax64(), 96 * MiB) << invalid;
        EXPECT_EQ(effective()["settings"]["gdalCacheMb"], 96) << invalid;
    }
    // The limits
    ASSERT_DDB_OK(DDBConfigure(R"({"gdalCacheMb": 16})"));
    EXPECT_EQ(GDALGetCacheMax64(), 16 * MiB);
    ASSERT_DDB_OK(DDBConfigure(R"({"gdalCacheMb": 1048576})"));
    EXPECT_EQ(GDALGetCacheMax64(), 1048576 * MiB);
}

}  // namespace
