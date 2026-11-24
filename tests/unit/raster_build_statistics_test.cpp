/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The statistics of a built COG: DDBBuildRasterStatistics writes them next to a COG that has none (one built
// before libddb wrote them at build time), the raster.cog artifact says whether it has them, and
// DDBGetRasterInfo lists the formulas of a raster without its statistics. The statistics of the uploaded
// file: the COG build counts them, DDBBuildRasterStatistics writes them for an older build, and
// DDBGetRasterValueInfoEx2 reads them instead of the pixels.

#include <chrono>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "buildlock.h"
#include "helper_operations.h"
#include "raster_protocol.h"
#include "tilers_raster.h"
#include "dbops.h"
#include "ddb.h"
#include "gtest/gtest.h"
#include "json.h"
#include "mio.h"
#include "raster_stats.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"
#include "user_open_child.h"
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

    // The exact statistics of every band of a raster, read here (ddb-tilers): what the build is to count
    json exactBandsOf(const fs::path &file)
    {
        const auto raster = tilers::Raster::openPrivate(file);
        const auto source = rasterSourceOf(*raster);
        std::vector<int> bands(static_cast<size_t>(source->bandCount()));
        for (size_t i = 0; i < bands.size(); i++)
            bands[i] = static_cast<int>(i) + 1;
        const auto stats = computeExactStatistics(*source, bands, std::nullopt);
        StatisticsByBand byBand;
        for (size_t i = 0; i < bands.size(); i++)
            if (stats[i])
                byBand[bands[i]] = *stats[i];
        return statisticsByBandToJson(byBand);
    }

    json valueInfoOf(const RasterDataset &ds, const char *entryPath)
    {
        char *out = nullptr;
        EXPECT_EQ(DDBGetRasterValueInfoEx2(ds.root.c_str(), entryPath, nullptr, &out), DDBERR_NONE)
            << DDBGetLastError();
        const json info = json::parse(out != nullptr ? out : "null");
        DDBFree(out);
        return info;
    }

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

    // The build counts the statistics of the uploaded file in its pass: those of its own pixels (not of the COG,
    // which is warped and resampled), bit for bit, and the artifact says so
    TEST(RasterSourceStatistics, theBuildCountsTheStatisticsOfTheUploadedFile)
    {
        RasterDataset ds(TEST_NAME);
        EXPECT_FALSE(ds.cogArtifact().is_object()) << "not built yet";
        ASSERT_DDB_OK(DDBBuild(ds.root.c_str(), "ortho.tif", nullptr, false, false));
        EXPECT_TRUE(ds.cogArtifact().at("sourceStatistics").get<bool>());
        const json source = bandsOf(sourceStatsSidecarOf(ds.cog()));
        ASSERT_EQ(source.size(), 3u);
        EXPECT_EQ(source, exactBandsOf(ds.index.root / "ortho.tif"));
        EXPECT_EQ(source.at("1").at("total").get<uint64_t>(), 96u * 96u) << "the pixels of the file";
        EXPECT_NE(source, bandsOf(statsSidecarOf(ds.cog()))) << "not those of the warped COG";
    }

    // The value info of an entry takes the statistics of its build: no pixel is read for them, and the info is
    // the one of the file
    TEST(RasterSourceStatistics, theValueInfoOfAnEntryReadsNoPixelForItsStatistics)
    {
        RasterDataset ds(TEST_NAME);
        ASSERT_DDB_OK(DDBBuild(ds.root.c_str(), "ortho.tif", nullptr, false, false));
        const std::string file = (ds.index.root / "ortho.tif").string();
        char *out = nullptr;
        ASSERT_DDB_OK(DDBGetRasterValueInfoEx(file.c_str(), nullptr, &out));
        const json read = json::parse(out);
        DDBFree(out);

        RasterStatsProvider::instance().clear();
        const auto before = RasterStatsProvider::instance().counters();
        EXPECT_EQ(valueInfoOf(ds, "ortho.tif"), read);
        const auto after = RasterStatsProvider::instance().counters();
        EXPECT_EQ(after.computed, before.computed) << "no pass over the pixels";
        EXPECT_EQ(after.fromSidecar, before.fromSidecar) << "not the sidecar of a copy either";

        // A NoData override is another key: it is computed
        DDBRasterOptions options{};
        options.structSize = sizeof(options);
        options.flags = DDB_RASTER_OPT_NODATA;
        options.nodata = 0.0;
        ASSERT_DDB_OK(DDBGetRasterValueInfoEx2(ds.root.c_str(), "ortho.tif", &options, &out));
        DDBFree(out);
        EXPECT_GT(RasterStatsProvider::instance().counters().computed + RasterStatsProvider::instance().counters().fromSidecar,
                  after.computed + after.fromSidecar);
    }

    // A file changed since it was indexed is not described by the statistics of its build: they are read again,
    // and DDBBuildRasterStatistics leaves them alone
    TEST(RasterSourceStatistics, aFileChangedSinceItWasIndexedIsReadAgain)
    {
        RasterDataset ds(TEST_NAME);
        ASSERT_DDB_OK(DDBBuild(ds.root.c_str(), "ortho.tif", nullptr, false, false));
        const fs::path file = ds.index.root / "ortho.tif";
        fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(10));
        RasterStatsProvider::instance().clear();
        const auto before = RasterStatsProvider::instance().counters();
        valueInfoOf(ds, "ortho.tif");
        const auto after = RasterStatsProvider::instance().counters();
        EXPECT_GT(after.computed + after.fromSidecar, before.computed + before.fromSidecar);

        fs::remove(sourceStatsSidecarOf(ds.cog()));
        json result;
        ASSERT_DDB_OK(ds.statistics("ortho.tif", nullptr, result));
        EXPECT_EQ(result, json({{"status", "present"}})) << "the COG has its own; the file is not the indexed one";
        EXPECT_FALSE(fs::exists(sourceStatsSidecarOf(ds.cog())));
    }

    // A build that did not count them (before 2.12) gets them from DDBBuildRasterStatistics: the raster helper
    // reads the uploaded file, with the values the build counts; with the statistics of the COG missing too,
    // both are written and the progress covers both passes
    TEST(RasterSourceStatistics, anOlderBuildGetsThemFromTheStatisticsCall)
    {
        RasterDataset ds(TEST_NAME);
        ASSERT_DDB_OK(DDBBuild(ds.root.c_str(), "ortho.tif", nullptr, false, false));
        const fs::path cog = ds.cog();
        const json built = bandsOf(sourceStatsSidecarOf(cog));
        fs::remove(sourceStatsSidecarOf(cog));
        EXPECT_FALSE(ds.cogArtifact().at("sourceStatistics").get<bool>());
        EXPECT_TRUE(ds.cogArtifact().at("statistics").get<bool>());

        json result;
        ASSERT_DDB_OK(ds.statistics("ortho.tif", R"({"threads": 2})", result));
        EXPECT_EQ(result, json({{"status", "computed"}}));
        EXPECT_EQ(bandsOf(sourceStatsSidecarOf(cog)), built) << "the values of the build, bit for bit";
        EXPECT_TRUE(ds.cogArtifact().at("sourceStatistics").get<bool>());
        ASSERT_DDB_OK(ds.statistics("ortho.tif", nullptr, result));
        EXPECT_EQ(result, json({{"status", "present"}}));

        fs::remove(sourceStatsSidecarOf(cog));
        fs::remove(statsSidecarOf(cog));
        Progress progress;
        ASSERT_DDB_OK(ds.statistics("ortho.tif", nullptr, result, &Progress::report, &progress));
        EXPECT_EQ(result, json({{"status", "computed"}}));
        EXPECT_TRUE(ds.cogArtifact().at("statistics").get<bool>());
        EXPECT_TRUE(ds.cogArtifact().at("sourceStatistics").get<bool>());
        ASSERT_FALSE(progress.fractions.empty());
        for (size_t i = 1; i < progress.fractions.size(); i++)
            EXPECT_GE(progress.fractions[i], progress.fractions[i - 1]) << "monotonic";
        EXPECT_EQ(progress.fractions.back(), 1.0);

        // Statistics older than the COG are of another build
        fs::last_write_time(sourceStatsSidecarOf(cog), fs::last_write_time(cog) - std::chrono::seconds(10));
        EXPECT_FALSE(ds.cogArtifact().at("sourceStatistics").get<bool>());
    }

    // The source statistics of a JPEG 2000: the build counts them as the helper's statistics operation reads the
    // file (GDAL), the same values
    TEST(RasterSourceStatistics, aJpeg2000HasTheValuesTheHelperReads)
    {
        IndexFixture index(TEST_NAME);
        const std::string root = index.root.string();
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = 80;
        spec.bands = 1;
        spec.type = GDT_UInt16;
        spec.epsg = 32632;
        spec.originX = 500000.0;
        spec.originY = 4500000.0;
        spec.value = [](int, int x, int y) { return static_cast<double>(1000 + (x * 37 + y * 11) % 3001); };
        testutils::createGeoJp2(index.root / "dem.jp2", spec);
        addToIndex(index.db.get(), {(index.root / "dem.jp2").string()});
        ASSERT_DDB_OK(DDBBuild(root.c_str(), "dem.jp2", nullptr, false, false));

        const char *paths[] = {"dem.jp2"};
        char *out = nullptr;
        ASSERT_DDB_OK(DDBGetBuildArtifacts(root.c_str(), paths, 1, &out));
        const json build = json::parse(out).at(0).at("build");
        DDBFree(out);
        const fs::path cog = io::fromUtf8(build.at("root").get<std::string>()) / "cog" / "cog.tif";
        const json counted = bandsOf(sourceStatsSidecarOf(cog));

        EXPECT_EQ(counted.at("1").at("min").get<double>(), 1000.0);
        DDBReleaseConnections(root.c_str(), nullptr);
        testutils::runWithUserFileOpens([&] {
            const json read = raster::runHelperOperation(raster::op::kStatistics, index.root / "dem.jp2", fs::path(),
                                                         json::object(), raster::ProgressCallback());
            EXPECT_EQ(counted, read.at("bands"));
        });
    }

    // Statistics another process wrote are checked member by member before they are used
    TEST(RasterSourceStatistics, statisticsFromAnotherProcessAreChecked)
    {
        const json good = json::parse(R"({"1": {"min": 1, "max": 9, "mean": 4.5, "std": 2, "valid": 10, "total": 12}})");
        const StatisticsByBand parsed = statisticsByBandFromJson(good);
        ASSERT_EQ(parsed.size(), 1u);
        EXPECT_EQ(parsed.at(1).validCount, 10u);
        EXPECT_EQ(statisticsByBandToJson(parsed), good);
        EXPECT_TRUE(statisticsByBandFromJson(json::object()).empty());

        for (const char *bad : {
                 R"([])",
                 R"({"0": {"min": 1, "max": 9, "mean": 4.5, "std": 2, "valid": 10, "total": 12}})",
                 R"({"x": {"min": 1, "max": 9, "mean": 4.5, "std": 2, "valid": 10, "total": 12}})",
                 R"({"1": {"max": 9, "mean": 4.5, "std": 2, "valid": 10, "total": 12}})",
                 R"({"1": {"min": "1", "max": 9, "mean": 4.5, "std": 2, "valid": 10, "total": 12}})",
                 R"({"1": {"min": 1, "max": 9, "mean": 10, "std": 2, "valid": 10, "total": 12}})",
                 R"({"1": {"min": 1, "max": 9, "mean": 4.5, "std": -1, "valid": 10, "total": 12}})",
                 R"({"1": {"min": 1, "max": 9, "mean": 4.5, "std": 2, "valid": 13, "total": 12}})",
                 R"({"1": {"min": 1, "max": 9, "mean": 4.5, "std": 2, "valid": 0, "total": 12}})",
                 R"({"1": {"min": 1, "max": 9, "mean": 4.5, "std": 2, "valid": -1, "total": 12}})",
                 R"({"1": 5})"})
            EXPECT_THROW(statisticsByBandFromJson(json::parse(bad)), InvalidArgsException) << bad;
    }
}
