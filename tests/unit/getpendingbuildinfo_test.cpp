/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "build.h"
#include "timeutil.h"
#include "dbops.h"
#include "ddb.h"
#include "exceptions.h"
#include "gtest/gtest.h"
#include "json.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

#include <chrono>
#include <fstream>

namespace {

using namespace ddb;

// Test suite for getPendingBuildInfo() / DDBGetPendingBuildInfo, the
// read-only view over ".pending" marker files used to explain to callers
// why a build has been deferred.
class GetPendingBuildInfoTest : public ::testing::Test {
protected:
    void SetUp() override {
        testArea = std::make_unique<TestArea>(
            "GetPendingBuildInfoTest-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        dbPath = testArea->getPath("");
        ddb::initIndex(dbPath.string());
        db = ddb::open(dbPath.string(), true);

        vectorPath = testArea->getPath("layer.shp");
        std::ofstream(vectorPath) << "fake shapefile bytes";
        ddb::addToIndex(db.get(), {vectorPath.string()});
    }

    void TearDown() override {
        db.reset();
        testArea.reset();
    }

    // Hash of the indexed file at `absolutePath` (utils.h indexedHash)
    std::string indexedHash(const fs::path& absolutePath) {
        return ::indexedHash(db.get(), fs::relative(absolutePath, dbPath).string());
    }

    // Writes a ".pending" marker file with the given timestamp and missing
    // dependency names, mirroring the format written by buildInternal().
    static fs::path writePendingFile(const fs::path& buildDir,
                                      const std::string& hash,
                                      const std::string& timestampLine,
                                      const std::vector<std::string>& deps) {
        const fs::path p = buildDir / (hash + ".pending");
        fs::create_directories(buildDir);
        std::ofstream f(p);
        f << timestampLine << std::endl;
        for (const auto& dep : deps)
            f << dep << std::endl;
        return p;
    }

    std::unique_ptr<TestArea> testArea;
    fs::path dbPath;
    std::unique_ptr<Database> db;
    fs::path vectorPath;
};

TEST_F(GetPendingBuildInfoTest, NoBuildDirectoryReturnsEmpty) {
    // buildDirectory() has not been created yet (no build attempted)
    EXPECT_TRUE(ddb::getPendingBuildInfo(db.get()).empty());
}

TEST_F(GetPendingBuildInfoTest, NoPendingFilesReturnsEmpty) {
    fs::create_directories(db->buildDirectory());
    EXPECT_TRUE(ddb::getPendingBuildInfo(db.get()).empty());
}

TEST_F(GetPendingBuildInfoTest, ReturnsHashPathDepsAndTimestamp) {
    const auto hash = indexedHash(vectorPath);
    const time_t ts = 1721612345;
    writePendingFile(db->buildDirectory(), hash, std::to_string(ts), {"layer.dbf", "layer.prj"});

    const auto pending = ddb::getPendingBuildInfo(db.get());
    ASSERT_EQ(pending.size(), 1u);

    const auto rel = fs::relative(vectorPath, dbPath).string();
    EXPECT_EQ(pending[0].hash, hash);
    EXPECT_EQ(pending[0].path, rel);
    EXPECT_EQ(pending[0].lastAttempt, ts);
    EXPECT_EQ(pending[0].retryAfter, ts + 60) << "lastAttempt + the 60 s backoff of buildPending()";
    ASSERT_EQ(pending[0].missingDependencies.size(), 2u);
    EXPECT_EQ(pending[0].missingDependencies[0], "layer.dbf");
    EXPECT_EQ(pending[0].missingDependencies[1], "layer.prj");
}

TEST_F(GetPendingBuildInfoTest, DoesNotConsumeOrModifyThePendingFile) {
    const auto hash = indexedHash(vectorPath);
    const auto pendingPath = writePendingFile(db->buildDirectory(), hash, "0", {"layer.dbf"});

    ddb::getPendingBuildInfo(db.get());

    EXPECT_TRUE(fs::exists(pendingPath));
    // Calling it again must yield the same result (read-only, idempotent)
    const auto pending = ddb::getPendingBuildInfo(db.get());
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending[0].missingDependencies.size(), 1u);
}

TEST_F(GetPendingBuildInfoTest, EntryNoLongerInDbIsSkipped) {
    // A stray ".pending" file whose hash matches nothing in the database
    // (e.g. the entry was removed after the build was deferred).
    writePendingFile(db->buildDirectory(), "0000000000000000000000000000000000000000000000000000000000000000", "0", {"missing.dbf"});

    EXPECT_TRUE(ddb::getPendingBuildInfo(db.get()).empty());
}

// Same marker set as buildPending(): only top-level "<hash>.pending" files.
// Nested markers (e.g. inside an output folder) and non-hash names are ignored
// by getPendingBuildInfo() and isBuildPending() alike.
TEST_F(GetPendingBuildInfoTest, OnlyTopLevelHashMarkersCount) {
    const auto hash = indexedHash(vectorPath);
    const fs::path buildDir = db->buildDirectory();

    // Without dependencies: isBuildPending() would count any of them it collected
    writePendingFile(buildDir / hash, hash, "0", {});
    writePendingFile(buildDir / "sub" / "deeper", hash, "0", {});
    writePendingFile(buildDir, "notahash", "0", {});
    writePendingFile(buildDir, hash.substr(0, 63), "0", {});
    fs::create_directories(buildDir / (hash + "x.pending"));  // a directory, not a marker

    EXPECT_TRUE(ddb::getPendingBuildInfo(db.get()).empty());
    EXPECT_FALSE(ddb::isBuildPending(db.get()));

    writePendingFile(buildDir, hash, "0", {"layer.dbf"});
    const auto pending = ddb::getPendingBuildInfo(db.get());
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending[0].hash, hash);
    ASSERT_EQ(pending[0].missingDependencies.size(), 1u);
    EXPECT_EQ(pending[0].missingDependencies[0], "layer.dbf");
    EXPECT_FALSE(ddb::isBuildPending(db.get())) << "it awaits a file";

    writePendingFile(buildDir, hash, "0", {});
    EXPECT_TRUE(ddb::isBuildPending(db.get()));
}

// awaitingFiles is true exactly when the marker lists missing dependencies; both kinds are
// listed, only the other kind counts for isBuildPending()
TEST_F(GetPendingBuildInfoTest, AwaitingFilesWhenDependenciesAreListed) {
    const auto hash = indexedHash(vectorPath);
    writePendingFile(db->buildDirectory(), hash, "1721612345", {"layer.dbf"});
    auto pending = ddb::getPendingBuildInfo(db.get());
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_TRUE(pending[0].awaitingFiles);
    EXPECT_FALSE(ddb::isBuildPending(db.get()));

    writePendingFile(db->buildDirectory(), hash, "1721612345", {});
    pending = ddb::getPendingBuildInfo(db.get());
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_FALSE(pending[0].awaitingFiles);
    EXPECT_TRUE(pending[0].missingDependencies.empty());
    EXPECT_EQ(pending[0].retryAfter, 1721612345 + 60);
    EXPECT_TRUE(ddb::isBuildPending(db.get()));
}

TEST_F(GetPendingBuildInfoTest, MalformedTimestampToleratedAsZero) {
    const auto hash = indexedHash(vectorPath);
    writePendingFile(db->buildDirectory(), hash, "not-a-timestamp", {"layer.dbf"});

    const auto pending = ddb::getPendingBuildInfo(db.get());
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending[0].lastAttempt, 0);
    EXPECT_EQ(pending[0].retryAfter, 60);
    EXPECT_LE(pending[0].retryAfter, ddb::currentUnixTimestamp()) << "a marker without a time is due at once";
    ASSERT_EQ(pending[0].missingDependencies.size(), 1u);
    EXPECT_EQ(pending[0].missingDependencies[0], "layer.dbf");
}

TEST_F(GetPendingBuildInfoTest, DoesNotTriggerBuildOrApplyBackoff) {
    // Even a very recent timestamp (well within the 60-second retry backoff
    // used by buildPending()) must still be reported: this is a pure query,
    // not a retry attempt.
    const auto hash = indexedHash(vectorPath);
    const time_t now = ddb::currentUnixTimestamp();
    writePendingFile(db->buildDirectory(), hash, std::to_string(now), {"layer.dbf"});

    const auto pending = ddb::getPendingBuildInfo(db.get());
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending[0].missingDependencies.size(), 1u);
}

// retryAfter is when buildPending() retries a build that awaits no file: not before it, and at
// it. layer.shp still lacks its .shx, so a retry defers it again and refreshes the marker.
TEST_F(GetPendingBuildInfoTest, RetryAfterTellsWhenBuildPendingRetries) {
    const auto hash = indexedHash(vectorPath);

    // Attempted 10 s ago: the retry is 50 s away, and buildPending() leaves the marker alone
    const time_t recent = ddb::currentUnixTimestamp() - 10;
    writePendingFile(db->buildDirectory(), hash, std::to_string(recent), {});
    auto pending = ddb::getPendingBuildInfo(db.get());
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending[0].retryAfter, recent + 60);
    EXPECT_GT(pending[0].retryAfter, ddb::currentUnixTimestamp());
    ddb::buildPending(db.get(), "", false);
    pending = ddb::getPendingBuildInfo(db.get());
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending[0].lastAttempt, recent) << "retried before retryAfter";
    EXPECT_TRUE(pending[0].missingDependencies.empty());

    // Attempted exactly the backoff ago: retryAfter has come, and buildPending() retries
    const time_t due = ddb::currentUnixTimestamp() - 60;
    writePendingFile(db->buildDirectory(), hash, std::to_string(due), {});
    pending = ddb::getPendingBuildInfo(db.get());
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending[0].retryAfter, due + 60);
    EXPECT_LE(pending[0].retryAfter, ddb::currentUnixTimestamp());
    const time_t beforeRetry = ddb::currentUnixTimestamp();
    ddb::buildPending(db.get(), "", false);
    pending = ddb::getPendingBuildInfo(db.get());
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_GE(pending[0].lastAttempt, beforeRetry) << "not retried at retryAfter";
    EXPECT_EQ(pending[0].missingDependencies, std::vector<std::string>{"layer.shx"});
    EXPECT_EQ(pending[0].retryAfter, pending[0].lastAttempt + 60);
    EXPECT_TRUE(pending[0].awaitingFiles);
}

// --- C API round-trip ---

TEST_F(GetPendingBuildInfoTest, CApiReturnsValidJson) {
    const auto hash = indexedHash(vectorPath);
    writePendingFile(db->buildDirectory(), hash, "1721612345", {"layer.dbf"});

    char* output = nullptr;
    ASSERT_EQ(DDBGetPendingBuildInfo(dbPath.string().c_str(), &output), DDBERR_NONE);
    ASSERT_NE(output, nullptr);

    const auto j = json::parse(std::string(output));
    DDBFree(output);

    ASSERT_TRUE(j.is_array());
    ASSERT_EQ(j.size(), 1u);
    EXPECT_EQ(j[0]["hash"].get<std::string>(), hash);
    EXPECT_EQ(j[0]["lastAttempt"].get<long long>(), 1721612345LL);
    ASSERT_TRUE(j[0]["retryAfter"].is_number_integer());
    EXPECT_EQ(j[0]["retryAfter"].get<long long>(), 1721612405LL);
    ASSERT_TRUE(j[0]["missingDependencies"].is_array());
    EXPECT_EQ(j[0]["missingDependencies"][0].get<std::string>(), "layer.dbf");
    ASSERT_TRUE(j[0]["awaitingFiles"].is_boolean());
    EXPECT_TRUE(j[0]["awaitingFiles"].get<bool>());

    writePendingFile(db->buildDirectory(), hash, "1721612345", {});
    ASSERT_EQ(DDBGetPendingBuildInfo(dbPath.string().c_str(), &output), DDBERR_NONE);
    const auto released = json::parse(std::string(output));
    DDBFree(output);
    ASSERT_EQ(released.size(), 1u);
    EXPECT_FALSE(released[0]["awaitingFiles"].get<bool>());
}

TEST_F(GetPendingBuildInfoTest, CApiReturnsEmptyArrayWhenNothingPending) {
    char* output = nullptr;
    ASSERT_EQ(DDBGetPendingBuildInfo(dbPath.string().c_str(), &output), DDBERR_NONE);
    ASSERT_NE(output, nullptr);

    const auto j = json::parse(std::string(output));
    DDBFree(output);

    ASSERT_TRUE(j.is_array());
    EXPECT_TRUE(j.empty());
}

}  // namespace
