/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The ordering invariant between a build attempt that defers a build and the add of a file it
// waits for: a dependency that lands (and is indexed) after the builder checked it but before the
// marker is written finds no marker to release; the attempt checks the disk again after its
// write and releases the marker itself. The window is opened with the test seam of the attempt
// (setMissingDependencyHookForTesting), so the interleaving is deterministic.

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "build.h"
#include "build_internal.h"
#include "dbops.h"
#include "exceptions.h"
#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace {

using namespace ddb;

const std::string kObjBody =
    "v 0 0 0\nv 1 0 0\nv 1 1 0\nvt 0 0\nvt 1 0\nvt 1 1\nusemtl mat\nf 1/1 2/2 3/3\n";

struct Dataset : IndexFixture {
    explicit Dataset(const std::string& name) : IndexFixture(name, "") {}

    ~Dataset() { setMissingDependencyHookForTesting(nullptr); }

    fs::path path(const std::string& rel) const { return root / fs::path(rel); }

    void add(const std::vector<std::string>& rels) {
        std::vector<std::string> abs;
        for (const auto& r : rels)
            abs.push_back(path(r).string());
        addToIndex(db.get(), abs);
    }

    // The marker of rel is in its released form: due at once, awaiting no file
    bool released(const std::string& rel) {
        std::ifstream in(db->buildDirectory() / (indexedHash(db.get(), rel) + ".pending"), std::ios::binary);
        return in.is_open() && std::string(std::istreambuf_iterator<char>(in), {}) == "0\n";
    }
};

TEST(PendingRecheck, DependencyArrivingBeforeTheMarkerIsWrittenReleasesIt) {
    Dataset ds(TEST_NAME);
    writeFile(ds.path("m.obj"), "mtllib m.mtl\n" + kObjBody);
    ds.add({"m.obj"});

    // The upload of the MTL completes in the window: on disk, indexed, released (no marker yet)
    setMissingDependencyHookForTesting([&ds] {
        writeFile(ds.path("m.mtl"), "newmtl mat\nKd 1 1 1\n");
        ds.add({"m.mtl"});
    });
    EXPECT_THROW(build(ds.db.get(), "m.obj", "", false), BuildDepMissingException);
    setMissingDependencyHookForTesting(nullptr);

    EXPECT_TRUE(ds.released("m.obj"));
    EXPECT_TRUE(isBuildPending(ds.db.get()));
}

// The re-check looks where the builder looked: an MTL texture reported as "mats/tex.png" is also
// satisfied by tex.png beside the OBJ
TEST(PendingRecheck, DependencyArrivingAtAnotherLookupPathReleasesIt) {
    Dataset ds(TEST_NAME);
    writeFile(ds.path("m.obj"), "mtllib mats/m.mtl\n" + kObjBody);
    writeFile(ds.path("mats/m.mtl"), "newmtl mat\nKd 1 1 1\nmap_Kd tex.png\n");
    ds.add({"m.obj", "mats/m.mtl"});

    // On disk only: its add comes later
    setMissingDependencyHookForTesting([&ds] { writeFile(ds.path("tex.png"), "png"); });
    EXPECT_THROW(build(ds.db.get(), "m.obj", "", false), BuildDepMissingException);
    setMissingDependencyHookForTesting(nullptr);

    EXPECT_TRUE(ds.released("m.obj"));
}

// The check after the builder's report looks only where the builder looks: a file it finds and the
// builder does not would release a build that the builder defers again, retried forever.
// a/a/m.mtl is where the reported name "a/m.mtl" would land if it were resolved against the
// folder of the OBJ once more.
TEST(PendingRecheck, AFileTheBuilderDoesNotUseDoesNotReleaseIt) {
    Dataset ds(TEST_NAME);
    writeFile(ds.path("a/m.obj"), "mtllib m.mtl\n" + kObjBody);
    writeFile(ds.path("a/a/m.mtl"), "newmtl mat\nKd 1 1 1\n");
    ds.add({"a/m.obj"});

    EXPECT_THROW(build(ds.db.get(), "a/m.obj", "", false), BuildDepMissingException);
    EXPECT_FALSE(ds.released("a/m.obj"));
    const auto info = getPendingBuildInfo(ds.db.get());
    ASSERT_EQ(info.size(), 1u);
    EXPECT_EQ(info[0].missingDependencies, std::vector<std::string>{"a/m.mtl"});
    EXPECT_TRUE(info[0].awaitingFiles);
    EXPECT_FALSE(isBuildPending(ds.db.get()));
}

// Nothing arrives in the window: the marker awaits the file, and no timer work is announced
TEST(PendingRecheck, NothingArrivingLeavesTheMarkerAwaiting) {
    Dataset ds(TEST_NAME);
    writeFile(ds.path("m.obj"), "mtllib m.mtl\n" + kObjBody);
    ds.add({"m.obj"});

    bool called = false;
    setMissingDependencyHookForTesting([&called] { called = true; });
    EXPECT_THROW(build(ds.db.get(), "m.obj", "", false), BuildDepMissingException);
    setMissingDependencyHookForTesting(nullptr);

    EXPECT_TRUE(called);
    EXPECT_FALSE(ds.released("m.obj"));
    const auto info = getPendingBuildInfo(ds.db.get());
    ASSERT_EQ(info.size(), 1u);
    EXPECT_TRUE(info[0].awaitingFiles);
    EXPECT_FALSE(isBuildPending(ds.db.get()));
}

}  // namespace
