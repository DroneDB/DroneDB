/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DDB-08 — the GDAL block cache is bounded during COG builds and by default
// in the process, and the build thread budget is configurable.

#include <mutex>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "build_budget.h"
#include "cog.h"
#include "gdal_inc.h"
#include "test.h"
#include "testarea.h"
#include "testfs.h"

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

    TEST(DDB08_Budget, defaultsAndOverrides)
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
        EXPECT_EQ(b.gdalNumThreads(), std::to_string(b.threads));

        CPLSetConfigOption("DDB_COG_CACHE_MB", "256");
        CPLSetConfigOption("DDB_WARP_MEMORY_MB", "128");
        CPLSetConfigOption("DDB_BUILD_THREADS", "0");
        b = BuildResourceBudget::fromConfig();
        EXPECT_EQ(b.cacheBytes, 256 * MiB);
        EXPECT_EQ(b.warpMemoryMb, 128);
        EXPECT_EQ(b.gdalNumThreads(), "ALL_CPUS");

        // Invalid values fall back to the defaults.
        CPLSetConfigOption("DDB_COG_CACHE_MB", "-5");
        CPLSetConfigOption("DDB_BUILD_THREADS", "-1");
        b = BuildResourceBudget::fromConfig();
        EXPECT_EQ(b.cacheBytes, 1024 * MiB);
        EXPECT_GE(b.threads, 1);
    }

    TEST(DDB08_Budget, scopedCacheLowersAndRestores)
    {
        ConfigSnapshot snap({});
        GDALSetCacheMax64(8192 * MiB);
        {
            const ScopedGdalCacheMax guard(1024 * MiB);
            EXPECT_EQ(GDALGetCacheMax64(), 1024 * MiB);
        }
        EXPECT_EQ(GDALGetCacheMax64(), 8192 * MiB);
    }

    // Two builds overlap in one process: the first one to finish must not put the
    // unbounded cache back while the second one is still running.
    TEST(DDB08_Budget, overlappingGuardsStayBoundedUntilTheLastEnds)
    {
        ConfigSnapshot snap({});
        GDALSetCacheMax64(8192 * MiB);

        auto *a = new ScopedGdalCacheMax(1024 * MiB);
        auto *b = new ScopedGdalCacheMax(512 * MiB);
        EXPECT_EQ(GDALGetCacheMax64(), 1024 * MiB);   // largest live request
        delete a;
        EXPECT_EQ(GDALGetCacheMax64(), 512 * MiB);    // b still running: stays bounded
        delete b;
        EXPECT_EQ(GDALGetCacheMax64(), 8192 * MiB);   // last guard restores the original
    }

    TEST(DDB08_Budget, defaultBoundLowersLargeCache)
    {
        ConfigSnapshot snap({"GDAL_CACHEMAX", "DDB_GDAL_CACHE_MB"});
        CPLSetConfigOption("GDAL_CACHEMAX", nullptr);
        CPLSetConfigOption("DDB_GDAL_CACHE_MB", nullptr);

        GDALSetCacheMax64(6144 * MiB);   // GDAL default on a 123 GB host
        applyDefaultGdalCacheBound();
        EXPECT_EQ(GDALGetCacheMax64(), 1024 * MiB);

        GDALSetCacheMax64(400 * MiB);    // small host: never raised
        applyDefaultGdalCacheBound();
        EXPECT_EQ(GDALGetCacheMax64(), 400 * MiB);
    }

    TEST(DDB08_Cog, respectsUserCacheMax)
    {
        ConfigSnapshot snap({"GDAL_CACHEMAX"});
        CPLSetConfigOption("GDAL_CACHEMAX", "8192");
        GDALSetCacheMax64(8192 * MiB);
        applyDefaultGdalCacheBound();
        EXPECT_EQ(GDALGetCacheMax64(), 8192 * MiB);
    }

    // Samples GDALGetCacheMax64() from GDAL debug messages emitted inside the
    // warp kernel, i.e. while the warp is running, on whichever thread runs it.
    struct WarpCacheProbe
    {
        static std::mutex mutex;
        static std::vector<GIntBig> samples;
        static void CPL_STDCALL handler(CPLErr cls, CPLErrorNum, const char *msg)
        {
            if (cls != CE_Debug || msg == nullptr || std::string(msg).find("GDALWarpKernel") == std::string::npos)
                return;
            std::lock_guard<std::mutex> lock(mutex);
            samples.push_back(GDALGetCacheMax64());
        }
    };
    std::mutex WarpCacheProbe::mutex;
    std::vector<GIntBig> WarpCacheProbe::samples;

    TEST(DDB08_Cog, cacheIsBoundedDuringBuild)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path input = ta.downloadTestAsset(
            testDataUrl("ortho/brighton-beach.tif"), "brighton-beach.tif");

        ConfigSnapshot snap({"DDB_COG_CACHE_MB", "CPL_DEBUG"});
        CPLSetConfigOption("DDB_COG_CACHE_MB", "64");
        CPLSetConfigOption("CPL_DEBUG", "ON");
        GDALSetCacheMax64(8192 * MiB);

        WarpCacheProbe::samples.clear();
        CPLErrorHandler prev = CPLSetErrorHandler(WarpCacheProbe::handler);
        buildCog(input.string(), ta.getPath("cog.tif").string());
        CPLSetErrorHandler(prev);

        ASSERT_FALSE(WarpCacheProbe::samples.empty()) << "no warp kernel debug message observed";
        for (const auto v : WarpCacheProbe::samples)
            EXPECT_EQ(v, 64 * MiB);
        EXPECT_EQ(GDALGetCacheMax64(), 8192 * MiB) << "previous cache size must be restored";
    }
} // namespace
