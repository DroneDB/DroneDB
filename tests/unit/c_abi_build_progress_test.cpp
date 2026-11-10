/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DDBBuildEx2 (new in 2.4, FR-PRC-42): DDBBuildEx with a progress callback.
//
//  - the fractions it reports never go down and the last one of a build that succeeds is 1;
//  - a raster (COG) build reports the phases "warping", "overviews" and "compressing", in that order;
//  - another kind of entry reports its type, with the fraction -1 until it is built;
//  - a build of several entries reports the fraction of the entries done plus the fraction of the one in
//    the build;
//  - a callback that returns non-zero cancels the build with DDBERR_CANCELED and a message that names it,
//    and the entry keeps no build.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "build_layout.h"
#include "cog_utils.h"
#include "dbops.h"
#include "ddb.h"
#include "entry_handler.h"
#include "gtest/gtest.h"
#include "json.h"
#include "raster_helper.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"

namespace {

using namespace ddb;
namespace fs = std::filesystem;

#define ASSERT_DDB_OK(call) ASSERT_EQ((call), DDBERR_NONE) << #call << ": " << DDBGetLastError()

// What the callback received
struct Report {
    double fraction;
    std::string phase;  // "<null>" for a NULL phase
};

struct Recorder {
    std::vector<Report> reports;
    // Non-zero: the callback returns 1 from the first report whose fraction is at least this one
    double cancelFrom = -2.0;
    // Non-empty: the callback returns 1 from the first report of this phase
    std::string cancelAtPhase;
    bool canceled = false;
};

int record(double fraction, const char* phase, void* userData) {
    auto* recorder = static_cast<Recorder*>(userData);
    recorder->reports.push_back({fraction, phase != nullptr ? phase : "<null>"});
    if ((recorder->cancelFrom > -2.0 && fraction >= recorder->cancelFrom) ||
        (!recorder->cancelAtPhase.empty() && recorder->cancelAtPhase == (phase != nullptr ? phase : ""))) {
        recorder->canceled = true;
        return 1;
    }
    return 0;
}

// A dataset with the given files copied in and indexed
struct Dataset {
    TestArea area;
    fs::path root;

    explicit Dataset(const std::string& name) : area(name, true), root(area.getFolder("ds")) {
        char* out = nullptr;
        EXPECT_EQ(DDBInit(root.string().c_str(), &out), DDBERR_NONE) << DDBGetLastError();
        DDBFree(out);
    }

    void add(const fs::path& file) {
        const std::string path = file.string();
        const char* paths[] = {path.c_str()};
        char* out = nullptr;
        ASSERT_EQ(DDBAdd(root.string().c_str(), paths, 1, &out, false), DDBERR_NONE) << DDBGetLastError();
        DDBFree(out);
    }

    fs::path addRaster(const std::string& name, int size = 1024) {
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = size;
        const auto file = testutils::createGeoRaster(root / name, spec);
        add(file);
        return file;
    }

    // The pending marker of an entry, as a build that was interrupted left it (due at once, no file awaited)
    fs::path writePending(const std::string& entry) {
        const auto db = ddb::open(root.string(), true);
        const auto e = getEntry(db.get(), entry);
        EXPECT_TRUE(e.has_value());
        const fs::path marker = pendingMarkerPath(db->buildDirectory(), entryHandler(e->type).buildKey(*e), entry);
        fs::create_directories(marker.parent_path());
        std::ofstream(marker) << "0\n";
        return marker;
    }

    bool isBuildComplete(const std::string& entry) {
        bool complete = false;
        EXPECT_EQ(DDBIsBuildComplete(root.string().c_str(), entry.c_str(), &complete), DDBERR_NONE)
            << DDBGetLastError();
        return complete;
    }
};

// The known fractions never go down, everything is in [-1, 1], and a -1 comes only before the first known one
void expectMonotonic(const std::vector<Report>& reports) {
    double last = -1.0;
    for (const auto& r : reports) {
        EXPECT_GE(r.fraction, -1.0);
        EXPECT_LE(r.fraction, 1.0);
        if (last >= 0.0) {
            EXPECT_GE(r.fraction, last) << "the fraction went down at phase " << r.phase;
        }
        last = std::max(last, r.fraction);
    }
}

// The position of each phase of `order` among the reports must follow that order
void expectPhasesInOrder(const std::vector<Report>& reports, const std::vector<std::string>& order) {
    size_t next = 0;
    for (const auto& r : reports) {
        const auto found = std::find(order.begin(), order.end(), r.phase);
        if (found == order.end())
            continue;
        const size_t index = static_cast<size_t>(found - order.begin());
        EXPECT_GE(index, next) << "phase " << r.phase << " came after a later one";
        next = std::max(next, index);
    }
}

bool hasPhase(const std::vector<Report>& reports, const std::string& phase) {
    for (const auto& r : reports)
        if (r.phase == phase)
            return true;
    return false;
}

TEST(DDBBuildEx2, aRasterBuildReportsMonotonicProgressAndItsPhases) {
    Dataset ds("DDBBuildEx2-raster");
    ds.addRaster("ortho.tif");

    Recorder recorder;
    ASSERT_DDB_OK(DDBBuildEx2(ds.root.string().c_str(), "ortho.tif", nullptr, true, false, nullptr, &record, &recorder));

    ASSERT_GE(recorder.reports.size(), 3u);
    expectMonotonic(recorder.reports);
    EXPECT_EQ(recorder.reports.back().fraction, 1.0);
    EXPECT_TRUE(hasPhase(recorder.reports, "warping")) << "no warping phase";
    EXPECT_TRUE(hasPhase(recorder.reports, "compressing")) << "no compressing phase";
    expectPhasesInOrder(recorder.reports, {"georaster", "warping", "overviews", "compressing", "publishing"});

    // The first report is the one of the raster kind with nothing measured yet, and a known fraction
    // comes with the phase GDAL's progress is in: the first third is the warp
    EXPECT_EQ(recorder.reports.front().fraction, -1.0);
    EXPECT_EQ(recorder.reports.front().phase, "georaster");
    for (const auto& r : recorder.reports)
    {
        if (r.phase == "warping" && r.fraction >= 0.0) {
            EXPECT_LT(r.fraction, 0.375);
        } else if (r.phase == "compressing") {
            EXPECT_GE(r.fraction, 0.5);
        }
    }

    EXPECT_TRUE(ds.isBuildComplete("ortho.tif"));
}

// The warp is announced when it starts, whatever GDAL reports during it: a small raster warped in one GDAL chunk
// goes from 0 straight to its overviews, and its build still says "warping" (right after the raster kind, at 0)
TEST(DDBBuildEx2, aRasterBuildAnnouncesTheWarpWhenItStarts) {
    Dataset ds("DDBBuildEx2-warp-start");
    ds.addRaster("small.tif", 64);

    Recorder recorder;
    ASSERT_DDB_OK(DDBBuildEx2(ds.root.string().c_str(), "small.tif", nullptr, true, false, nullptr, &record, &recorder));

    ASSERT_GE(recorder.reports.size(), 3u);
    EXPECT_EQ(recorder.reports[0].fraction, -1.0);
    EXPECT_EQ(recorder.reports[0].phase, "georaster");
    EXPECT_EQ(recorder.reports[1].fraction, 0.0);
    EXPECT_EQ(recorder.reports[1].phase, "warping");
    expectMonotonic(recorder.reports);
    expectPhasesInOrder(recorder.reports, {"georaster", "warping", "overviews", "compressing", "publishing"});
    EXPECT_EQ(recorder.reports.back().fraction, 1.0);
    EXPECT_TRUE(ds.isBuildComplete("small.tif"));
}

TEST(DDBBuildEx2, withoutACallbackItIsDDBBuildEx) {
    Dataset ds("DDBBuildEx2-no-callback");
    ds.addRaster("ortho.tif", 256);

    ASSERT_DDB_OK(DDBBuildEx2(ds.root.string().c_str(), "ortho.tif", nullptr, true, false, R"({"threads": 2})", nullptr, nullptr));
    EXPECT_TRUE(ds.isBuildComplete("ortho.tif"));
}

TEST(DDBBuildEx2, aCallbackThatReturnsNonZeroCancelsTheBuild) {
    Dataset ds("DDBBuildEx2-cancel");
    ds.addRaster("ortho.tif", 2048);

    Recorder recorder;
    recorder.cancelFrom = 0.01;  // the first report of the warp
    EXPECT_EQ(DDBBuildEx2(ds.root.string().c_str(), "ortho.tif", nullptr, true, false, nullptr, &record, &recorder),
              DDBERR_CANCELED);
    EXPECT_TRUE(recorder.canceled) << "the callback was never asked to cancel";
    const std::string message = DDBGetLastError();
    EXPECT_NE(message.find("progress callback"), std::string::npos) << message;
    EXPECT_FALSE(ds.isBuildComplete("ortho.tif")) << "a canceled build must leave no build";

    // The cancellation is that of the call: the same build then runs to the end
    Recorder again;
    ASSERT_DDB_OK(DDBBuildEx2(ds.root.string().c_str(), "ortho.tif", nullptr, true, false, nullptr, &record, &again));
    EXPECT_TRUE(ds.isBuildComplete("ortho.tif"));
}

TEST(DDBBuildEx2, aBuildOfSeveralEntriesReportsTheEntriesDonePlusTheOneInTheBuild) {
    Dataset ds("DDBBuildEx2-several");
    ds.addRaster("a.tif", 256);
    ds.addRaster("b.tif", 256);

    Recorder recorder;
    ASSERT_DDB_OK(DDBBuildEx2(ds.root.string().c_str(), nullptr, nullptr, true, false, nullptr, &record, &recorder));

    expectMonotonic(recorder.reports);
    EXPECT_EQ(recorder.reports.back().fraction, 1.0);
    // The first entry is half of the work: it is done at 0.5 (no phase), and nothing it reports is above it
    // until then
    size_t half = recorder.reports.size();
    for (size_t i = 0; i < recorder.reports.size(); i++)
        if (recorder.reports[i].fraction == 0.5 && recorder.reports[i].phase == "<null>") {
            half = i;
            break;
        }
    ASSERT_LT(half, recorder.reports.size()) << "no report of the first entry done";
    for (size_t i = 0; i < half; i++)
        EXPECT_LE(recorder.reports[i].fraction, 0.5);
    // The second entry reports in the upper half
    bool upper = false;
    for (size_t i = half + 1; i < recorder.reports.size(); i++)
        upper = upper || (recorder.reports[i].fraction > 0.5 && recorder.reports[i].phase != "<null>");
    EXPECT_TRUE(upper);
    EXPECT_TRUE(ds.isBuildComplete("a.tif"));
    EXPECT_TRUE(ds.isBuildComplete("b.tif"));
}

TEST(DDBBuildEx2, aBuildThatHasNothingToDoReportsTheEnd) {
    Dataset ds("DDBBuildEx2-nothing");

    Recorder all;
    ASSERT_DDB_OK(DDBBuildEx2(ds.root.string().c_str(), nullptr, nullptr, false, false, nullptr, &record, &all));
    ASSERT_FALSE(all.reports.empty());
    EXPECT_EQ(all.reports.back().fraction, 1.0);

    Recorder pending;
    ASSERT_DDB_OK(DDBBuildEx2(ds.root.string().c_str(), nullptr, nullptr, false, true, nullptr, &record, &pending));
    ASSERT_FALSE(pending.reports.empty());
    EXPECT_EQ(pending.reports.back().fraction, 1.0);
}

TEST(DDBBuildEx2, anEntryOfAnotherKindReportsItsTypeUntilItIsBuilt) {
    Dataset ds("DDBBuildEx2-vector");
    const auto geojson = ds.root / "area.geojson";
    std::ofstream(geojson) << R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{},)"
                              R"("geometry":{"type":"Polygon","coordinates":[[[10,45],[10.1,45],[10.1,45.1],[10,45]]]}}]})";
    ds.add(geojson);

    Recorder recorder;
    ASSERT_DDB_OK(DDBBuildEx2(ds.root.string().c_str(), "area.geojson", nullptr, true, false, nullptr, &record, &recorder));

    ASSERT_GE(recorder.reports.size(), 2u);
    EXPECT_EQ(recorder.reports.front().fraction, -1.0);
    EXPECT_EQ(recorder.reports.front().phase, "vector");
    expectMonotonic(recorder.reports);
    EXPECT_EQ(recorder.reports.back().fraction, 1.0);
    EXPECT_TRUE(ds.isBuildComplete("area.geojson"));
}

// Nothing but the end of a build that succeeds is 1: the output is made at the last fraction the build reached
TEST(DDBBuildEx2, onlyTheEndOfASuccessfulBuildIsOne) {
    Dataset ds("DDBBuildEx2-end");
    ds.addRaster("ortho.tif", 256);

    Recorder recorder;
    ASSERT_DDB_OK(DDBBuildEx2(ds.root.string().c_str(), "ortho.tif", nullptr, true, false, nullptr, &record, &recorder));

    ASSERT_GE(recorder.reports.size(), 3u);
    EXPECT_EQ(recorder.reports.back().fraction, 1.0);
    EXPECT_EQ(recorder.reports.back().phase, "<null>");
    size_t publishing = recorder.reports.size();
    for (size_t i = 0; i + 1 < recorder.reports.size(); i++) {
        EXPECT_LT(recorder.reports[i].fraction, 1.0) << "report " << i << " (" << recorder.reports[i].phase << ")";
        if (recorder.reports[i].phase == "publishing")
            publishing = i;
    }
    ASSERT_LT(publishing, recorder.reports.size()) << "no publishing report";
    EXPECT_EQ(publishing + 2, recorder.reports.size()) << "the 1 follows the publish";
    // Not 1, and the last fraction of the build
    EXPECT_GT(recorder.reports[publishing].fraction, 0.5);
    EXPECT_LT(recorder.reports[publishing].fraction, 1.0);
}

// A cancel at the report of the publish stops the build before anything is published, and a pending build
// stays pending
TEST(DDBBuildEx2, aCancelWhenTheOutputIsMadePublishesNothingAndKeepsThePending) {
    Dataset ds("DDBBuildEx2-cancel-publish");
    ds.addRaster("ortho.tif", 256);
    const fs::path marker = ds.writePending("ortho.tif");
    ASSERT_TRUE(fs::exists(marker));

    Recorder recorder;
    recorder.cancelAtPhase = "publishing";
    EXPECT_EQ(DDBBuildEx2(ds.root.string().c_str(), "ortho.tif", nullptr, false, false, nullptr, &record, &recorder),
              DDBERR_CANCELED);
    EXPECT_TRUE(recorder.canceled);
    EXPECT_EQ(recorder.reports.back().phase, "publishing") << "nothing is reported after the cancel";
    EXPECT_LT(recorder.reports.back().fraction, 1.0);
    const std::string message = DDBGetLastError();
    EXPECT_NE(message.find("progress callback"), std::string::npos) << message;

    EXPECT_FALSE(ds.isBuildComplete("ortho.tif")) << "the output was published";
    EXPECT_TRUE(fs::exists(marker)) << "a pending build stays pending";
    for (const auto& item : fs::recursive_directory_iterator(marker.parent_path()))
        EXPECT_NE(item.path().filename(), "cog.tif") << "published: " << item.path();

    // The next call builds it and consumes the marker
    Recorder again;
    ASSERT_DDB_OK(DDBBuildEx2(ds.root.string().c_str(), "ortho.tif", nullptr, false, false, nullptr, &record, &again));
    EXPECT_TRUE(ds.isBuildComplete("ortho.tif"));
    EXPECT_FALSE(fs::exists(marker));
}

// A raster that is on the grid already is encoded again without a warp: it never reports "warping"
TEST(DDBBuildEx2, aRasterOnTheGridAlreadyNeverReportsWarping) {
    // A COG made by a build is on the grid (Web Mercator, tiles, overviews)
    fs::path onTheGrid;
    {
        Dataset source("DDBBuildEx2-grid-source");
        source.addRaster("source.tif", 1024);
        ASSERT_DDB_OK(DDBBuildEx2(source.root.string().c_str(), "source.tif", nullptr, true, false, nullptr, nullptr, nullptr));
        for (const auto& item : fs::recursive_directory_iterator(source.root / ".ddb" / "build"))
            if (item.path().filename() == "cog.tif")
                onTheGrid = item.path();
        ASSERT_FALSE(onTheGrid.empty());
    }
    Dataset ds("DDBBuildEx2-grid");
    const fs::path file = ds.root / "grid.tif";
    fs::copy_file(onTheGrid, file);
    ASSERT_TRUE(raster::rasterHelper().probe(file, raster::ProbeParams()).isCog) << "the file is not on the grid";
    ds.add(file);

    Recorder recorder;
    ASSERT_DDB_OK(DDBBuildEx2(ds.root.string().c_str(), "grid.tif", nullptr, true, false, nullptr, &record, &recorder));

    expectMonotonic(recorder.reports);
    EXPECT_EQ(recorder.reports.back().fraction, 1.0);
    EXPECT_FALSE(hasPhase(recorder.reports, "warping")) << "a raster without a warp reported one";
    EXPECT_TRUE(hasPhase(recorder.reports, "compressing"));
    expectPhasesInOrder(recorder.reports, {"georaster", "overviews", "compressing", "publishing"});
    for (const auto& r : recorder.reports) {
        if (r.phase == "overviews") {
            EXPECT_LT(r.fraction, 0.2);
        } else if (r.phase == "compressing") {
            EXPECT_GE(r.fraction, 0.2);
        }
    }
    EXPECT_TRUE(ds.isBuildComplete("grid.tif"));
}

// The pending markers are the slices of the call: the fraction of the markers done plus the one in the build
TEST(DDBBuildEx2, aBuildOfThePendingOnesReportsTheFractionAcrossTheMarkers) {
    Dataset ds("DDBBuildEx2-pending");
    ds.addRaster("a.tif", 256);
    ds.addRaster("b.tif", 288);
    const fs::path markerA = ds.writePending("a.tif");
    const fs::path markerB = ds.writePending("b.tif");

    Recorder recorder;
    ASSERT_DDB_OK(DDBBuildEx2(ds.root.string().c_str(), nullptr, nullptr, false, true, nullptr, &record, &recorder));

    expectMonotonic(recorder.reports);
    EXPECT_EQ(recorder.reports.back().fraction, 1.0);
    // The first marker is half of the work: it ends at 0.5 (no phase), and the second one reports in the upper half
    size_t half = recorder.reports.size();
    for (size_t i = 0; i < recorder.reports.size(); i++)
        if (recorder.reports[i].fraction == 0.5 && recorder.reports[i].phase == "<null>") {
            half = i;
            break;
        }
    ASSERT_LT(half, recorder.reports.size()) << "no report of the first marker done";
    for (size_t i = 0; i < half; i++)
        EXPECT_LE(recorder.reports[i].fraction, 0.5);
    bool upper = false;
    for (size_t i = half + 1; i < recorder.reports.size(); i++)
        upper = upper || (recorder.reports[i].fraction > 0.5 && recorder.reports[i].phase == "compressing");
    EXPECT_TRUE(upper);
    EXPECT_TRUE(ds.isBuildComplete("a.tif"));
    EXPECT_TRUE(ds.isBuildComplete("b.tif"));
    EXPECT_FALSE(fs::exists(markerA));
    EXPECT_FALSE(fs::exists(markerB));
}

// A callback that cancels once the first entry is done stops the call before the next one
TEST(DDBBuildEx2, aCancelDuringABuildOfSeveralEntriesStopsBeforeTheNext) {
    Dataset ds("DDBBuildEx2-cancel-several");
    ds.addRaster("a.tif", 256);
    ds.addRaster("b.tif", 288);

    Recorder recorder;
    recorder.cancelFrom = 0.5;  // the first entry is done: half of the work
    EXPECT_EQ(DDBBuildEx2(ds.root.string().c_str(), nullptr, nullptr, true, false, nullptr, &record, &recorder),
              DDBERR_CANCELED);
    EXPECT_TRUE(recorder.canceled);
    EXPECT_NE(std::string(DDBGetLastError()).find("progress callback"), std::string::npos);
    EXPECT_NE(ds.isBuildComplete("a.tif"), ds.isBuildComplete("b.tif")) << "exactly the first entry is built";
    EXPECT_LE(recorder.reports.back().fraction, 0.5);
}

// A model (a point cloud) reports its type first, like a vector
TEST(DDBBuildEx2, aModelBuildReportsItsType) {
    Dataset ds("DDBBuildEx2-model");
    const auto obj = ds.root / "m.obj";
    std::ofstream(obj) << "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3\nf 1 3 4\n";
    ds.add(obj);

    Recorder recorder;
    ASSERT_DDB_OK(DDBBuildEx2(ds.root.string().c_str(), "m.obj", nullptr, true, false, nullptr, &record, &recorder));

    ASSERT_GE(recorder.reports.size(), 2u);
    EXPECT_EQ(recorder.reports.front().fraction, -1.0);
    EXPECT_EQ(recorder.reports.front().phase, "model");
    expectMonotonic(recorder.reports);
    EXPECT_EQ(recorder.reports.back().fraction, 1.0);
    EXPECT_TRUE(ds.isBuildComplete("m.obj"));
}

TEST(DDBBuildEx2, aPointCloudBuildReportsItsTypeBeforeAnythingElse) {
    Dataset ds("DDBBuildEx2-pointcloud");
    const auto xyz = ds.root / "cloud.xyz";
    {
        std::ofstream out(xyz);
        for (int i = 0; i < 50; i++)
            out << i % 7 << " " << i % 5 << " " << i % 3 << "\n";
    }
    ds.add(xyz);

    // The type is the first report; the callback stops the build there, so the test does not wait for the tools
    Recorder recorder;
    recorder.cancelAtPhase = "pointcloud";
    EXPECT_EQ(DDBBuildEx2(ds.root.string().c_str(), "cloud.xyz", nullptr, true, false, nullptr, &record, &recorder),
              DDBERR_CANCELED);
    ASSERT_FALSE(recorder.reports.empty());
    EXPECT_EQ(recorder.reports.front().fraction, -1.0);
    EXPECT_EQ(recorder.reports.front().phase, "pointcloud");
    EXPECT_FALSE(ds.isBuildComplete("cloud.xyz"));
}

TEST(DDBBuildEx2, refusesWhatDDBBuildExRefuses) {
    Dataset ds("DDBBuildEx2-args");
    Recorder recorder;

    EXPECT_EQ(DDBBuildEx2(nullptr, nullptr, nullptr, false, false, nullptr, &record, &recorder), DDBERR_INVALIDARGS);
    EXPECT_EQ(DDBBuildEx2("", nullptr, nullptr, false, false, nullptr, &record, &recorder), DDBERR_INVALIDARGS);
    EXPECT_EQ(DDBBuildEx2(ds.root.string().c_str(), nullptr, nullptr, false, false, R"({"threads": 0})", &record, &recorder),
              DDBERR_INVALIDARGS);
    EXPECT_EQ(DDBBuildEx2(ds.root.string().c_str(), nullptr, nullptr, false, false, R"({"unknown": 1})", &record, &recorder),
              DDBERR_INVALIDARGS);
    EXPECT_TRUE(recorder.reports.empty()) << "a refused call reported progress";

    // An entry that is not in the index is not found (DDBBuildEx answers DDBERR_EXCEPTION, with the same message)
    EXPECT_EQ(DDBBuildEx2(ds.root.string().c_str(), "missing.tif", nullptr, false, false, nullptr, &record, &recorder),
              DDBERR_NOTFOUND);
    EXPECT_STREQ(DDBGetLastError(), "missing.tif is not a valid path in the database.");
    EXPECT_EQ(DDBBuildEx(ds.root.string().c_str(), "missing.tif", nullptr, false, false, nullptr), DDBERR_EXCEPTION);
    EXPECT_STREQ(DDBGetLastError(), "missing.tif is not a valid path in the database.");
    EXPECT_TRUE(recorder.reports.empty());
}

// The phases of a COG build are those of the fraction GDAL's COG driver is at
TEST(cogPhaseOfFraction, followsTheWorkOfTheCogDriver) {
    EXPECT_STREQ(cogPhaseOfFraction(0.0, true), "warping");
    EXPECT_STREQ(cogPhaseOfFraction(0.37, true), "warping");
    EXPECT_STREQ(cogPhaseOfFraction(0.375, true), "overviews");
    EXPECT_STREQ(cogPhaseOfFraction(0.49, true), "overviews");
    EXPECT_STREQ(cogPhaseOfFraction(0.5, true), "compressing");
    EXPECT_STREQ(cogPhaseOfFraction(1.0, true), "compressing");
}

// A raster that is on the grid already is encoded again without a warp: its overviews are 1/5 of the work
TEST(cogPhaseOfFraction, withoutAWarpThereIsNoWarpingPhase) {
    EXPECT_STREQ(cogPhaseOfFraction(0.0, false), "overviews");
    EXPECT_STREQ(cogPhaseOfFraction(0.19, false), "overviews");
    EXPECT_STREQ(cogPhaseOfFraction(0.2, false), "compressing");
    EXPECT_STREQ(cogPhaseOfFraction(0.45, false), "compressing");
    EXPECT_STREQ(cogPhaseOfFraction(1.0, false), "compressing");
}

}  // namespace
