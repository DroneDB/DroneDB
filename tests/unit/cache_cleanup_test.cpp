/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The cleanup of the cache and of the temporary folder (cache_cleanup.h, DDBCleanupCache): what goes by
// age and kind, what stays, the counts, a dry run, the options

#include <chrono>
#include <fstream>
#include <string>

#include "cache_cleanup.h"
#include "ddb.h"
#include "exceptions.h"
#include "gtest/gtest.h"
#include "json.h"
#include "mio.h"
#include "raster_artefacts.h"
#include "scoped_temp_folder.h"
#include "temp_names.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace
{
    using namespace ddb;
    using namespace std::chrono_literals;

    // A file of `bytes` bytes, last changed `age` ago (with its folders)
    fs::path aged(const fs::path &file, size_t bytes, std::chrono::hours age)
    {
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << std::string(bytes, 'x');
        const auto when = fs::file_time_type::clock::now() - age;
        fs::last_write_time(file, when);
        for (fs::path folder = file.parent_path(); folder.filename() != "cache" && folder.filename() != "tmp" &&
                                                   folder.has_relative_path();
             folder = folder.parent_path())
            fs::last_write_time(folder, when);
        return file;
    }

    // A cache and a temporary folder of the test, apart from the user's
    class CacheCleanup : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            area_ = std::make_unique<TestArea>(std::string("CacheCleanup.") +
                                                   ::testing::UnitTest::GetInstance()->current_test_info()->name(),
                                               true);
            cache_ = area_->getFolder("cache");
            temp_ = area_->getFolder("tmp");
            config_ = std::make_unique<ScopedProcessConfig>(json{{"cacheDir", io::toUtf8(cache_)}}.dump());
            tempFolder_ = std::make_unique<testutils::ScopedTempFolder>(temp_);
        }

        void TearDown() override
        {
            tempFolder_.reset();
            config_.reset();
        }

        std::unique_ptr<TestArea> area_;
        fs::path cache_, temp_;
        std::unique_ptr<ScopedProcessConfig> config_;
        std::unique_ptr<testutils::ScopedTempFolder> tempFolder_;
    };

    TEST_F(CacheCleanup, eachKindGoesAfterItsAgeAndTheRestStays)
    {
        // Old enough for its kind
        const fs::path oldTiles = aged(cache_ / "tiles" / "a1b2" / "18" / "1" / "2.png", 100, 6 * 24h);
        const fs::path oldDownload = aged(cache_ / "tiles" / "c3d4-e5f6.tif", 200, 8 * 24h);
        const fs::path oldDsm = aged(cache_ / "dsm_service_cache" / "n45e009.tif", 300, 31 * 24h);
        const fs::path leftRequest = aged(cache_ / "requests" / "cog-left" / "cog.tif", 400, 3 * 24h);
        const fs::path leftTemp = aged(temp_ / (std::string(tempnames::kLegacyGltfConvert) + "x") / "a.glb", 500, 3 * 24h);
        // A file, not a folder: the temporary COG of the GDAL driver of earlier versions
        const fs::path leftCog = aged(temp_ / (std::string(tempnames::kLegacyCogBuild) + "7_3"), 700, 3 * 24h);
        const fs::path legacy = aged(cache_ / "artefacts" / "normalized" / "0123.tif", 600, 1h);
        // Not old enough, not libddb's, or in use
        const fs::path newTiles = aged(cache_ / "tiles" / "b2c3" / "18" / "1" / "2.png", 10, 4 * 24h);
        const fs::path newDownload = aged(cache_ / "tiles" / "d4e5-f6a7.tif", 10, 6 * 24h);
        const fs::path newDsm = aged(cache_ / "dsm_service_cache" / "n46e009.tif", 10, 29 * 24h);
        const fs::path otherTemp = aged(temp_ / "somebody-else" / "a.txt", 10, 30 * 24h);
        const fs::path otherCog = aged(temp_ / "mycog.tif_7_3", 10, 30 * 24h);
        const fs::path newTemp = aged(temp_ / (std::string(tempnames::kRender) + "y") / "a.tif", 10, 1h);
        // A folder whose work still writes below it is not old, however old the folder itself
        const fs::path busy = temp_ / (std::string(tempnames::kAssimpConvert) + "z");
        aged(busy / "old.obj", 10, 5 * 24h);
        aged(busy / "sub" / "new.obj", 10, 1h);
        fs::last_write_time(busy, fs::file_time_type::clock::now() - 5 * 24h);
        const raster::RequestFolder live("live-");
        aged(live.path() / "cog.tif", 10, 5 * 24h);
        fs::last_write_time(live.path(), fs::file_time_type::clock::now() - 5 * 24h);

        const CacheCleanupResult result = cleanupCache();

        EXPECT_FALSE(result.dryRun);
        const auto expect = [&](const char *kind, uint64_t entries, uint64_t bytes) {
            EXPECT_EQ(result.kinds.at(kind).entries, entries) << kind;
            EXPECT_EQ(result.kinds.at(kind).bytes, bytes) << kind;
        };
        expect("tiles", 1, 100);
        expect("downloads", 1, 200);
        expect("dsm", 1, 300);
        expect("requests", 1, 400);
        expect("temp", 2, 1200);
        expect("artefacts", 1, 600);
        for (const fs::path &gone : {fs::path(cache_ / "tiles" / "a1b2"), oldDownload, oldDsm, leftRequest.parent_path(),
                                     leftTemp.parent_path(), leftCog, fs::path(cache_ / "artefacts")})
            EXPECT_FALSE(fs::exists(gone)) << gone;
        for (const fs::path &kept : {newTiles, newDownload, newDsm, otherTemp, otherCog, newTemp, busy / "old.obj",
                                     fs::path(live.path() / "cog.tif")})
            EXPECT_TRUE(fs::exists(kept)) << kept;
    }

    TEST_F(CacheCleanup, aDryRunCountsAndRemovesNothing)
    {
        const fs::path oldDownload = aged(cache_ / "tiles" / "c3d4-e5f6.tif", 200, 8 * 24h);
        const fs::path legacy = aged(cache_ / "artefacts" / "a.tif", 600, 1h);
        CacheCleanupOptions options;
        options.dryRun = true;
        const CacheCleanupResult result = cleanupCache(options);
        EXPECT_TRUE(result.dryRun);
        EXPECT_EQ(result.kinds.at("downloads").entries, 1u);
        EXPECT_EQ(result.kinds.at("artefacts").bytes, 600u);
        EXPECT_TRUE(fs::exists(oldDownload));
        EXPECT_TRUE(fs::exists(legacy));
    }

    TEST_F(CacheCleanup, theAgesAreTheOptions)
    {
        const fs::path download = aged(cache_ / "tiles" / "c3d4-e5f6.tif", 10, 2 * 24h);
        CacheCleanupOptions options;
        options.downloadDays = 3;
        EXPECT_EQ(cleanupCache(options).kinds.at("downloads").entries, 0u);
        options.downloadDays = 1;
        EXPECT_EQ(cleanupCache(options).kinds.at("downloads").entries, 1u);
        EXPECT_FALSE(fs::exists(download));
    }

    TEST_F(CacheCleanup, anEmptyCacheHasNothingToRemoveAndMakesNoFolder)
    {
        const CacheCleanupResult result = cleanupCache();
        for (const auto &[kind, count] : result.kinds)
            EXPECT_EQ(count.entries, 0u) << kind;
        EXPECT_TRUE(fs::is_empty(cache_));
    }

    TEST_F(CacheCleanup, theExportTakesItsOptionsAsJsonAndRefusesBadOnes)
    {
        aged(cache_ / "tiles" / "c3d4-e5f6.tif", 10, 2 * 24h);
        char *out = nullptr;
        ASSERT_EQ(DDBCleanupCache(R"({"downloadDays": 1, "dryRun": true})", &out), DDBERR_NONE) << DDBGetLastError();
        const json result = json::parse(out);
        DDBFree(out);
        EXPECT_TRUE(result.at("dryRun").get<bool>());
        EXPECT_EQ(result.at("kinds").at("downloads").at("entries"), 1);

        ASSERT_EQ(DDBCleanupCache(nullptr, &out), DDBERR_NONE) << DDBGetLastError();
        DDBFree(out);
        for (const char *bad : {R"({"tileDays": -1})", R"({"tileDays": 36501})", R"({"dsmDays": "30"})",
                                R"({"dryRun": 1})", R"({"days": 3})", "[]", "not json"})
        {
            EXPECT_EQ(DDBCleanupCache(bad, &out), DDBERR_INVALIDARGS) << bad;
        }
        EXPECT_EQ(DDBCleanupCache(nullptr, nullptr), DDBERR_INVALIDARGS);
    }
} // namespace
