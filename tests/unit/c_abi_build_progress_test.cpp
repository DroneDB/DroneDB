/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DDBBuildEx2 (new in 2.3, FR-PRC-42): DDBBuildEx with a progress callback.
//
//  - the fractions it reports never go down and the last one of a build that succeeds is 1;
//  - a raster (COG) build reports the phases "warping", "overviews" and "compressing", in that order;
//  - another kind of entry reports its type, with the fraction -1 until it is built;
//  - a build of several entries reports the fraction of the entries done plus the fraction of the one in
//    the build;
//  - a callback that returns non-zero cancels the build with DDBERR_CANCELED and a message that names it,
//    and the entry keeps no build.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "cog_utils.h"
#include "ddb.h"
#include "gtest/gtest.h"
#include "json.h"
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
    bool canceled = false;
};

int record(double fraction, const char* phase, void* userData) {
    auto* recorder = static_cast<Recorder*>(userData);
    recorder->reports.push_back({fraction, phase != nullptr ? phase : "<null>"});
    if (recorder->cancelFrom > -2.0 && fraction >= recorder->cancelFrom) {
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
        if (last >= 0.0)
            EXPECT_GE(r.fraction, last) << "the fraction went down at phase " << r.phase;
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
        if (r.phase == "warping" && r.fraction >= 0.0)
            EXPECT_LT(r.fraction, 0.375);
        else if (r.phase == "compressing")
            EXPECT_GE(r.fraction, 0.5);

    EXPECT_TRUE(ds.isBuildComplete("ortho.tif"));
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
    EXPECT_STREQ(cogPhaseOfFraction(0.0), "warping");
    EXPECT_STREQ(cogPhaseOfFraction(0.37), "warping");
    EXPECT_STREQ(cogPhaseOfFraction(0.375), "overviews");
    EXPECT_STREQ(cogPhaseOfFraction(0.49), "overviews");
    EXPECT_STREQ(cogPhaseOfFraction(0.5), "compressing");
    EXPECT_STREQ(cogPhaseOfFraction(1.0), "compressing");
}

}  // namespace
