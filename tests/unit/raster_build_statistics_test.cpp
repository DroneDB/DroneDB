/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The statistics of a built COG: DDBBuildRasterStatistics writes them next to a COG that has none (one built
// before libddb wrote them at build time), the raster.cog artifact says whether it has them, and
// DDBGetRasterInfo lists the formulas of a raster without its statistics.

#include <chrono>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "buildlock.h"
#include "dbops.h"
#include "ddb.h"
#include "gtest/gtest.h"
#include "json.h"
#include "mio.h"
#include "raster_stats.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace
{
    using namespace ddb;

#define ASSERT_DDB_OK(call) ASSERT_EQ((call), DDBERR_NONE) << #call << ": " << DDBGetLastError()

    // A dataset with a georeferenced raster of three bands (ortho.tif) and a text file (a.txt)
    struct RasterDataset
    {
        IndexFixture index;
        std::string root;

        explicit RasterDataset(const std::string &name) : index(name), root(index.root.string())
        {
            testutils::GeoRasterSpec spec;
            spec.width = spec.height = 96;
            spec.bands = 3;
            spec.epsg = 32632;
            spec.originX = 500000.0;
            spec.originY = 4500000.0;
            spec.value = [](int band, int x, int y) { return static_cast<double>((x * (band + 2) + y * 3) % 251); };
            testutils::createGeoRaster(index.root / "ortho.tif", spec);
            writeFile(index.root / "a.txt", "alpha");
            addToIndex(index.db.get(), {(index.root / "ortho.tif").string(), (index.root / "a.txt").string()});
        }

        ~RasterDataset() { DDBReleaseConnections(root.c_str(), nullptr); }

        // The raster.cog artifact of ortho.tif, as DDBGetBuildArtifacts lists it; null when it has none
        json cogArtifact() const
        {
            const char *paths[] = {"ortho.tif"};
            char *out = nullptr;
            EXPECT_EQ(DDBGetBuildArtifacts(root.c_str(), paths, 1, &out), DDBERR_NONE) << DDBGetLastError();
            const json builds = json::parse(out != nullptr ? out : "[]");
            DDBFree(out);
            for (const auto &a : builds.at(0).at("build").at("artifacts"))
                if (a.at("role") == "raster.cog")
                    return a;
            return nullptr;
        }

        fs::path cog() const
        {
            const char *paths[] = {"ortho.tif"};
            char *out = nullptr;
            EXPECT_EQ(DDBGetBuildArtifacts(root.c_str(), paths, 1, &out), DDBERR_NONE) << DDBGetLastError();
            const json builds = json::parse(out != nullptr ? out : "[]");
            DDBFree(out);
            return io::fromUtf8(builds.at(0).at("build").at("root").get<std::string>()) /
                   io::fromUtf8(cogArtifact().at("path").get<std::string>());
        }

        DDBErr statistics(const char *path, const char *options, json &result, DDBProgressCallback progress = nullptr,
                          void *userData = nullptr) const
        {
            char *out = nullptr;
            const DDBErr rc = DDBBuildRasterStatistics(root.c_str(), path, options, progress, userData, &out);
            result = out != nullptr ? json::parse(out) : json(nullptr);
            DDBFree(out);
            return rc;
        }
    };

    json bandsOf(const fs::path &sidecar)
    {
        std::ifstream in(sidecar);
        return json::parse(in).at("bands");
    }

    struct Progress
    {
        std::vector<double> fractions;
        std::vector<std::string> phases;
        int stopAt = 0; // the call that asks to stop, 0 for none

        static int report(double fraction, const char *phase, void *userData)
        {
            auto *p = static_cast<Progress *>(userData);
            p->fractions.push_back(fraction);
            p->phases.emplace_back(phase != nullptr ? phase : "");
            return p->stopAt != 0 && static_cast<int>(p->fractions.size()) >= p->stopAt ? 1 : 0;
        }
    };

    // A COG built before libddb wrote its statistics: the call writes them, the same values the build writes
    // (exact sums: integer samples give the same bits), the artifact says so, and a second call reads nothing
    TEST(RasterBuildStatistics, aCogWithoutStatisticsGetsTheStatisticsOfItsBuild)
    {
        RasterDataset ds(TEST_NAME);
        ASSERT_DDB_OK(DDBBuild(ds.root.c_str(), "ortho.tif", nullptr, false, false));
        ASSERT_TRUE(ds.cogArtifact().at("statistics").get<bool>()) << "the build writes them";
        const fs::path cog = ds.cog();
        const json built = bandsOf(statsSidecarOf(cog));
        ASSERT_EQ(built.size(), 3u);

        fs::remove(statsSidecarOf(cog));
        EXPECT_FALSE(ds.cogArtifact().at("statistics").get<bool>()) << "an old build has none";

        Progress progress;
        json result;
        ASSERT_DDB_OK(ds.statistics("ortho.tif", R"({"threads": 2})", result, &Progress::report, &progress));
        EXPECT_EQ(result, json({{"status", "computed"}}));
        EXPECT_EQ(bandsOf(statsSidecarOf(cog)), built) << "the values of the build, bit for bit";
        EXPECT_TRUE(ds.cogArtifact().at("statistics").get<bool>());
        ASSERT_FALSE(progress.fractions.empty());
        EXPECT_EQ(progress.fractions.back(), 1.0);
        for (const auto &phase : progress.phases)
            EXPECT_EQ(phase, "statistics");

        ASSERT_DDB_OK(ds.statistics("ortho.tif", nullptr, result));
        EXPECT_EQ(result, json({{"status", "present"}}));
    }

    // A sidecar older than the COG is not used: the artifact says the COG has no statistics
    TEST(RasterBuildStatistics, aSidecarOlderThanTheCogDoesNotCount)
    {
        RasterDataset ds(TEST_NAME);
        ASSERT_DDB_OK(DDBBuild(ds.root.c_str(), "ortho.tif", nullptr, false, false));
        const fs::path cog = ds.cog();
        const auto earlier = fs::last_write_time(cog) - std::chrono::seconds(10);
        fs::last_write_time(statsSidecarOf(cog), earlier);
        EXPECT_FALSE(ds.cogArtifact().at("statistics").get<bool>());
        json result;
        ASSERT_DDB_OK(ds.statistics("ortho.tif", nullptr, result));
        EXPECT_EQ(result, json({{"status", "computed"}}));
        EXPECT_TRUE(ds.cogArtifact().at("statistics").get<bool>());
    }

    // Nothing to do without a build; the arguments, the type of the entry, a running build and a stop
    TEST(RasterBuildStatistics, refusesWhatItCannotDoAndStopsWhenAsked)
    {
        RasterDataset ds(TEST_NAME);
        json result;
        ASSERT_DDB_OK(ds.statistics("ortho.tif", nullptr, result));
        EXPECT_EQ(result, json({{"status", "notBuilt"}}));

        EXPECT_EQ(ds.statistics("missing.tif", nullptr, result), DDBERR_NOTFOUND);
        EXPECT_EQ(ds.statistics("a.txt", nullptr, result), DDBERR_INVALIDARGS);
        for (const char *bad : {R"({"threads": -1})", R"({"threads": 1.5})", R"({"other": 1})", "[1]", "{"})
            EXPECT_EQ(ds.statistics("ortho.tif", bad, result), DDBERR_INVALIDARGS) << bad;
        EXPECT_EQ(result, json(nullptr)) << "no output on a failure";
        char *out = nullptr;
        EXPECT_EQ(DDBBuildRasterStatistics(ds.root.c_str(), "", nullptr, nullptr, nullptr, &out), DDBERR_INVALIDARGS);
        EXPECT_EQ(DDBBuildRasterStatistics(ds.root.c_str(), "ortho.tif", nullptr, nullptr, nullptr, nullptr),
                  DDBERR_INVALIDARGS);

        ASSERT_DDB_OK(DDBBuild(ds.root.c_str(), "ortho.tif", nullptr, false, false));
        const fs::path cog = ds.cog();
        fs::remove(statsSidecarOf(cog));
        {
            BuildLock running(io::toUtf8(cog.parent_path()));
            EXPECT_EQ(ds.statistics("ortho.tif", nullptr, result), DDBERR_BUILDINPROGRESS);
        }

        Progress stop;
        stop.stopAt = 1;
        EXPECT_EQ(ds.statistics("ortho.tif", nullptr, result, &Progress::report, &stop), DDBERR_CANCELED);
        EXPECT_FALSE(fs::exists(statsSidecarOf(cog))) << "a canceled call writes nothing";
        EXPECT_FALSE(ds.cogArtifact().at("statistics").get<bool>());
    }

    // DDBGetRasterInfo lists the formulas, colormaps and bands of DDBGetRasterMetadata without computing a statistic
    TEST(RasterBuildStatistics, rasterInfoListsTheFormulasWithoutStatistics)
    {
        RasterDataset ds(TEST_NAME);
        const std::string path = (ds.index.root / "ortho.tif").string();
        const auto before = RasterStatsProvider::instance().counters();
        char *out = nullptr;
        ASSERT_DDB_OK(DDBGetRasterInfo(path.c_str(), &out));
        const json info = json::parse(out);
        DDBFree(out);
        const auto after = RasterStatsProvider::instance().counters();
        EXPECT_EQ(after.computed, before.computed) << "no statistic computed";
        EXPECT_EQ(after.fromSidecar, before.fromSidecar);

        ASSERT_DDB_OK(DDBGetRasterMetadata(path.c_str(), nullptr, nullptr, &out));
        const json metadata = json::parse(out);
        DDBFree(out);
        EXPECT_EQ(RasterStatsProvider::instance().counters().computed, after.computed + 3) << "every band, once";
        for (const char *key : {"algorithms", "colormaps", "autoBands"})
            EXPECT_EQ(info.at(key), metadata.at(key)) << key;
        EXPECT_FALSE(info.at("algorithms").empty());
    }
}
