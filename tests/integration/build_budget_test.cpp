/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The GDAL block cache is bounded by default in the process, and the build budget (cache,
// warp memory, threads) is configurable.

#include <mutex>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "build_budget.h"
#include "cog.h"
#include "gdal_inc.h"
#include "in_process_raster_helper.h"
#include "parallel.h"
#include "raster_helper.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"
#include "testfs.h"
#include "user_open_child.h"

namespace
{
    using namespace ddb;

    constexpr int64_t MiB = 1024 * 1024;

    // Restores a CPL config option and the GDAL cache size on scope exit.
    class ConfigSnapshot
    {
    public:
        explicit ConfigSnapshot(std::vector<std::string> keys) : keys_(std::move(keys)), cache_(GDALGetCacheMax64())
        {
            for (const auto &k : keys_)
            {
                const char *v = CPLGetConfigOption(k.c_str(), nullptr);
                values_.emplace_back(v != nullptr, v != nullptr ? v : "");
            }
        }
        ~ConfigSnapshot()
        {
            for (size_t i = 0; i < keys_.size(); i++)
                CPLSetConfigOption(keys_[i].c_str(), values_[i].first ? values_[i].second.c_str() : nullptr);
            GDALSetCacheMax64(cache_);
        }

    private:
        std::vector<std::string> keys_;
        std::vector<std::pair<bool, std::string>> values_;
        GIntBig cache_;
    };

    TEST(GdalCacheBudget, defaultsAndOverrides)
    {
        ConfigSnapshot snap({"DDB_COG_CACHE_MB", "DDB_WARP_MEMORY_MB", "DDB_BUILD_THREADS"});
        CPLSetConfigOption("DDB_COG_CACHE_MB", nullptr);
        CPLSetConfigOption("DDB_WARP_MEMORY_MB", nullptr);
        CPLSetConfigOption("DDB_BUILD_THREADS", nullptr);

        auto b = BuildResourceBudget::fromConfig();
        EXPECT_EQ(b.cacheBytes, 1024 * MiB);
        EXPECT_EQ(b.warpMemoryMb, 512);
        EXPECT_GE(b.threads, 1);
        EXPECT_LE(b.threads, 8);

        CPLSetConfigOption("DDB_COG_CACHE_MB", "256");
        CPLSetConfigOption("DDB_WARP_MEMORY_MB", "128");
        CPLSetConfigOption("DDB_BUILD_THREADS", "0");
        b = BuildResourceBudget::fromConfig();
        EXPECT_EQ(b.cacheBytes, 256 * MiB);
        EXPECT_EQ(b.warpMemoryMb, 128);
        // 0 is every core, as a count: the CPU pool decides how many run (no ALL_CPUS)
        EXPECT_EQ(b.threads, static_cast<int>(parallel::hardwareThreads()));

        // Invalid values fall back to the defaults.
        CPLSetConfigOption("DDB_COG_CACHE_MB", "-5");
        CPLSetConfigOption("DDB_BUILD_THREADS", "-1");
        b = BuildResourceBudget::fromConfig();
        EXPECT_EQ(b.cacheBytes, 1024 * MiB);
        EXPECT_GE(b.threads, 1);
    }

    TEST(GdalCacheBudget, defaultBoundLowersLargeCache)
    {
        ConfigSnapshot snap({"GDAL_CACHEMAX", "DDB_GDAL_CACHE_MB"});
        CPLSetConfigOption("GDAL_CACHEMAX", nullptr);
        CPLSetConfigOption("DDB_GDAL_CACHE_MB", nullptr);

        GDALSetCacheMax64(6144 * MiB);   // GDAL default on a 123 GB host
        applyGdalCacheSetting();
        EXPECT_EQ(GDALGetCacheMax64(), 1024 * MiB);

        GDALSetCacheMax64(400 * MiB);    // small host: never raised
        applyGdalCacheSetting();
        EXPECT_EQ(GDALGetCacheMax64(), 400 * MiB);
    }

    TEST(CogCacheBudget, respectsUserCacheMax)
    {
        ConfigSnapshot snap({"GDAL_CACHEMAX"});
        CPLSetConfigOption("GDAL_CACHEMAX", "8192");
        GDALSetCacheMax64(8192 * MiB);
        applyGdalCacheSetting();
        EXPECT_EQ(GDALGetCacheMax64(), 8192 * MiB);
    }
} // namespace
