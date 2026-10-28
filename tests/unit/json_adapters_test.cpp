/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The logic that was in the C API adapter (ddb.cpp) and now lives in the library modules:
// the STAC request parameters (stac.cpp), the JSON of the build outputs (build_json.cpp) and of
// the merge validation (merge_multispectral_json.cpp), the verified hashes of a delta
// (delta_apply.cpp). The outputs are frozen by the contract fixtures; these pin the rules of the
// moved functions on their own.

#include <string>

#include "build.h"
#include "delta_apply.h"
#include "exceptions.h"
#include "gtest/gtest.h"
#include "json.h"
#include "merge_multispectral.h"
#include "stac.h"

namespace {

using namespace ddb;

TEST(StacQuery, bboxAndDatetime) {
    const auto none = parseStacItemQuery("", "", 0, -1);
    EXPECT_TRUE(none.bbox.empty());
    EXPECT_EQ(none.datetimeStart, "");
    EXPECT_EQ(none.datetimeEnd, "");
    EXPECT_EQ(none.limit, 0);
    EXPECT_EQ(none.offset, -1) << "the paging defaults are the generator's";

    // Blank is none, too
    EXPECT_TRUE(parseStacItemQuery("  ", " ", 10, 0).bbox.empty());

    const auto bbox = parseStacItemQuery("-10.5,20,30.25,40", "", 10, 5);
    EXPECT_EQ(bbox.bbox, (std::vector<double>{-10.5, 20, 30.25, 40}));
    EXPECT_EQ(bbox.limit, 10);
    EXPECT_EQ(bbox.offset, 5);

    // One instant: both bounds
    const auto instant = parseStacItemQuery("", "2024-05-01T10:00:00Z", 10, 0);
    EXPECT_EQ(instant.datetimeStart, "2024-05-01T10:00:00Z");
    EXPECT_EQ(instant.datetimeEnd, "2024-05-01T10:00:00Z");

    // An interval, with open ends
    const auto interval =
        parseStacItemQuery("", "2024-01-01T00:00:00Z/2024-12-31T23:59:59Z", 10, 0);
    EXPECT_EQ(interval.datetimeStart, "2024-01-01T00:00:00Z");
    EXPECT_EQ(interval.datetimeEnd, "2024-12-31T23:59:59Z");
    const auto openStart = parseStacItemQuery("", "../2024-12-31T23:59:59Z", 10, 0);
    EXPECT_EQ(openStart.datetimeStart, "");
    EXPECT_EQ(openStart.datetimeEnd, "2024-12-31T23:59:59Z");
    const auto openEnd = parseStacItemQuery("", "2024-01-01T00:00:00Z/..", 10, 0);
    EXPECT_EQ(openEnd.datetimeStart, "2024-01-01T00:00:00Z");
    EXPECT_EQ(openEnd.datetimeEnd, "");
    const auto emptyBound = parseStacItemQuery("", "/2024-12-31T23:59:59Z", 10, 0);
    EXPECT_EQ(emptyBound.datetimeStart, "");
}

TEST(StacQuery, invalidBboxIsInvalidArgs) {
    for (const char* bad : {"1,2,3", "1,2,3,4,5", "a,b,c,d", "1,2,,4", "1;2;3;4"}) {
        EXPECT_THROW(parseStacItemQuery(bad, "", 10, 0), InvalidArgsException) << bad;
    }
    try {
        parseStacItemQuery("1,2,x,4", "", 10, 0);
        FAIL();
    } catch (const InvalidArgsException& e) {
        EXPECT_STREQ(e.what(), "Invalid bbox value: x");
    }
    try {
        parseStacItemQuery("1,2,3", "", 10, 0);
        FAIL();
    } catch (const InvalidArgsException& e) {
        EXPECT_STREQ(e.what(), "bbox must contain exactly 4 comma-separated values");
    }
}

TEST(BuildJson, shapesOfTheBuildOutputs) {
    PendingBuildInfo pending;
    pending.hash = "h";
    pending.path = "a/b.shp";
    pending.missingDependencies = {"a/b.shx"};
    pending.lastAttempt = 1700000000;
    pending.retryAfter = 1700000060;
    pending.awaitingFiles = true;
    EXPECT_EQ(json(pending),
              json::parse(R"({"hash":"h","path":"a/b.shp","missingDependencies":["a/b.shx"],
        "lastAttempt":1700000000,"retryAfter":1700000060,"awaitingFiles":true})"));
    EXPECT_EQ(json(std::vector<PendingBuildInfo>{}).dump(), "[]");

    BuildStatus status;
    status.path = "x.tif";
    status.exists = true;
    status.complete = true;
    EXPECT_EQ(json(status),
              json::parse(R"({"path":"x.tif","exists":true,"buildable":false,"complete":true})"));

    CleanupResult cleanup;
    cleanup.removedEntries = {"gone.txt"};
    EXPECT_EQ(json(cleanup), json::parse(R"({"entries":["gone.txt"],"builds":[]})"));
}

TEST(MergeJson, validationShape) {
    MergeValidationResult r;
    r.ok = true;
    r.warnings = {"w"};
    r.summary.crs = "EPSG:32632";
    r.summary.width = 4;
    r.summary.height = 3;
    r.summary.totalBands = 5;
    r.summary.dataType = "UInt16";
    r.summary.pixelSizeX = 0.5;
    r.summary.pixelSizeY = 0.5;
    r.summary.estimatedSize = 1234;
    r.alignment.detected = true;
    r.alignment.maxShiftPixels = 1.5;
    r.alignment.shiftSource = "sensor";
    BandAlignmentInfo band;
    band.bandName = "Red";
    band.centralWavelength = 668;
    band.shiftX = 0.25;
    r.alignment.bands.push_back(band);

    const json j = json::parse(mergeValidationToJson(r));
    EXPECT_EQ(j["ok"], true);
    EXPECT_EQ(j["errors"], json::array());
    EXPECT_EQ(j["warnings"], json::array({"w"}));
    EXPECT_EQ(
        j["summary"],
        json::parse(R"({"totalBands":5,"dataType":"UInt16","width":4,"height":3,"crs":"EPSG:32632",
        "pixelSizeX":0.5,"pixelSizeY":0.5,"estimatedSize":1234})"));
    EXPECT_EQ(
        j["alignment"]["bands"][0],
        json::parse(R"({"index":0,"name":"Red","wavelength":668,"shiftX":0.25,"shiftY":0.0})"));
    EXPECT_EQ(j["alignment"]["maxShiftPixels"], 1.5);
    EXPECT_EQ(j["alignment"]["correctionApplied"], false);
}

TEST(DeltaJson, verifiedHashesAreAJsonObjectOfStrings) {
    const auto hashes = parseVerifiedHashes(json::parse(R"({"a.txt": "abc", "d/b.txt": "def"})"));
    EXPECT_EQ(hashes.size(), 2u);
    EXPECT_EQ(hashes.at("d/b.txt"), "def");
    EXPECT_TRUE(parseVerifiedHashes(json::object()).empty());

    EXPECT_THROW(parseVerifiedHashes(json::array()), InvalidArgsException);
    EXPECT_THROW(parseVerifiedHashes(json::parse(R"({"a.txt": 5})")), InvalidArgsException);
    try {
        parseVerifiedHashes(json::parse(R"({"a.txt": null})"));
        FAIL();
    } catch (const InvalidArgsException& e) {
        EXPECT_STREQ(e.what(), "The verified hash of a.txt must be a string");
    }
}

TEST(DeltaJson, aConflictIsItsPathInJson) {
    const std::vector<Conflict> conflicts = {
        Conflict("a.txt", ConflictType::BothModified),
        Conflict("b/c.txt", ConflictType::RemoteDeleteLocalModified)};
    EXPECT_EQ(json(conflicts).dump(), R"(["a.txt","b/c.txt"])");
}

}  // namespace
