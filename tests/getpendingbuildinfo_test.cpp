/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "build.h"
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

    std::string hashOf(const fs::path& absolutePath) {
        Entry e;
        const auto rel = fs::relative(absolutePath, dbPath);
        EXPECT_TRUE(ddb::getEntry(db.get(), rel.string(), e));
        return e.hash;
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
    const auto hash = hashOf(vectorPath);
    const time_t ts = 1721612345;
    writePendingFile(db->buildDirectory(), hash, std::to_string(ts), {"layer.dbf", "layer.prj"});

    const auto pending = ddb::getPendingBuildInfo(db.get());
    ASSERT_EQ(pending.size(), 1u);

    const auto rel = fs::relative(vectorPath, dbPath).string();
    EXPECT_EQ(pending[0].hash, hash);
    EXPECT_EQ(pending[0].path, rel);
    EXPECT_EQ(pending[0].lastAttempt, ts);
    ASSERT_EQ(pending[0].missingDependencies.size(), 2u);
    EXPECT_EQ(pending[0].missingDependencies[0], "layer.dbf");
    EXPECT_EQ(pending[0].missingDependencies[1], "layer.prj");
}

TEST_F(GetPendingBuildInfoTest, DoesNotConsumeOrModifyThePendingFile) {
    const auto hash = hashOf(vectorPath);
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

TEST_F(GetPendingBuildInfoTest, MalformedTimestampToleratedAsZero) {
    const auto hash = hashOf(vectorPath);
    writePendingFile(db->buildDirectory(), hash, "not-a-timestamp", {"layer.dbf"});

    const auto pending = ddb::getPendingBuildInfo(db.get());
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending[0].lastAttempt, 0);
    ASSERT_EQ(pending[0].missingDependencies.size(), 1u);
    EXPECT_EQ(pending[0].missingDependencies[0], "layer.dbf");
}

TEST_F(GetPendingBuildInfoTest, DoesNotTriggerBuildOrApplyBackoff) {
    // Even a very recent timestamp (well within the 5-minute backoff window
    // used by buildPending()) must still be reported: this is a pure query,
    // not a retry attempt.
    const auto hash = hashOf(vectorPath);
    const time_t now = utils::currentUnixTimestamp();
    writePendingFile(db->buildDirectory(), hash, std::to_string(now), {"layer.dbf"});

    const auto pending = ddb::getPendingBuildInfo(db.get());
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending[0].missingDependencies.size(), 1u);
}

// --- C API round-trip ---

TEST_F(GetPendingBuildInfoTest, CApiReturnsValidJson) {
    const auto hash = hashOf(vectorPath);
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
    ASSERT_TRUE(j[0]["missingDependencies"].is_array());
    EXPECT_EQ(j[0]["missingDependencies"][0].get<std::string>(), "layer.dbf");
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
