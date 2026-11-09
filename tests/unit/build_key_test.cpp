/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The build key (FR-XC-28, D-020; FR-PRC-23, FR-VWR-17): the folder of a build, the pending markers
// of its entries, the manifest, the cleanup and the rescan that moves an entry to its key.

#include <algorithm>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "build.h"
#include "build_internal.h"
#include "build_layout.h"
#include "build_manifest.h"
#include "dbops.h"
#include "entry_handler.h"
#include "exceptions.h"
#include "gtest/gtest.h"
#include "json.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace {

using namespace ddb;

const std::string kObjBody =
    "v 0 0 0\nv 1 0 0\nv 1 1 0\nvt 0 0\nvt 1 0\nvt 1 1\nusemtl mat\nf 1/1 2/2 3/3\n";
const std::string kMtl = "newmtl mat\nKd 1 1 1\n";

// The digest of {"kind":"enu","origin":[45.0,9.0,0.0]}, from
//   printf '%s' '{"kind":"enu","origin":[45.0,9.0,0.0]}' | sha256sum | cut -c1-16
const std::string kEnuDigest = "2edfb16a5dbc3259";

struct Dataset : IndexFixture {
    explicit Dataset(const std::string& name) : IndexFixture(name, "") {}

    fs::path path(const std::string& rel) const { return root / fs::path(rel); }

    void write(const std::string& rel, const std::string& content) const {
        fs::create_directories(path(rel).parent_path());
        std::ofstream(path(rel), std::ios::binary) << content;
    }

    void add(const std::vector<std::string>& rels) {
        std::vector<std::string> abs;
        for (const auto& r : rels)
            abs.push_back(path(r).string());
        addToIndex(db.get(), abs);
    }

    fs::path buildDir() const { return db->buildDirectory(); }

    std::vector<std::string> markerFiles() const {
        std::vector<std::string> names;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(buildDir(), ec))
            if (e.is_regular_file() && e.path().extension() == PendingMarkerExtension)
                names.push_back(e.path().filename().string());
        std::sort(names.begin(), names.end());
        return names;
    }

    std::map<std::string, PendingBuildInfo> pendingByPath() const {
        std::map<std::string, PendingBuildInfo> byPath;
        for (const auto& p : getPendingBuildInfo(db.get()))
            byPath[p.path] = p;
        return byPath;
    }

    // The build key of the entry as the index has it
    std::string keyOf(const std::string& rel) const {
        Entry e;
        EXPECT_TRUE(loadEntry(db.get(), rel, e)) << rel;
        return entryHandler(e.type).buildKey(e);
    }

    // Marks the entry as having the georeference of a local frame at lat 45, lon 9 (what the
    // indexing of a model or a splat next to a georef.json writes)
    void setGeoref(const std::string& rel) const {
        auto q = db->query("UPDATE entries SET properties = ? WHERE path = ?");
        q->bind(1, R"({"georef":{"kind":"enu","origin":[45.0,9.0,0.0]}})");
        q->bind(2, rel);
        q->execute();
    }
};

// ---- FR-PRC-23 ------------------------------------------------------------------------------------

// The same OBJ in two folders, without its .mtl: each copy has its own marker, naming its own folder
TEST(BuildKey, TwoIdenticalObjFilesInDifferentFoldersWaitEachForTheirOwnMtl) {
    Dataset ds(TEST_NAME);
    for (const char* rel : {"mixA/model.obj", "mixB/model.obj"})
        ds.write(rel, "mtllib brighton.mtl\n" + kObjBody);
    ds.add({"mixA/model.obj", "mixB/model.obj"});
    const std::string hash = hashOf(ds.db.get(), "mixA/model.obj");
    ASSERT_EQ(hash, hashOf(ds.db.get(), "mixB/model.obj"));

    EXPECT_THROW(build(ds.db.get(), "mixA/model.obj", "", false), BuildDepMissingException);
    EXPECT_THROW(build(ds.db.get(), "mixB/model.obj", "", false), BuildDepMissingException);

    // Two markers, named by the key and the path of their entries (the ids are sha256 of the path)
    EXPECT_EQ(ds.markerFiles(),
              (std::vector<std::string>{hash + ".2b7bc1b74ec754ab.pending", hash + ".46b6e6e1b32663ef.pending"}))
        << "mixA/model.obj, mixB/model.obj";

    const auto pending = ds.pendingByPath();
    ASSERT_EQ(pending.size(), 2u);
    EXPECT_EQ(pending.at("mixA/model.obj").missingDependencies, std::vector<std::string>{"mixA/brighton.mtl"});
    EXPECT_EQ(pending.at("mixB/model.obj").missingDependencies, std::vector<std::string>{"mixB/brighton.mtl"});
    EXPECT_EQ(pending.at("mixA/model.obj").key, hash);

    // The .mtl of mixA releases the marker of mixA only: mixB still waits for its own
    ds.write("mixA/brighton.mtl", kMtl);
    ds.add({"mixA/brighton.mtl"});
    const auto after = ds.pendingByPath();
    ASSERT_EQ(after.size(), 2u);
    EXPECT_FALSE(after.at("mixA/model.obj").awaitingFiles);
    EXPECT_TRUE(after.at("mixB/model.obj").awaitingFiles);
    EXPECT_EQ(after.at("mixB/model.obj").missingDependencies, std::vector<std::string>{"mixB/brighton.mtl"});

    // The manifest reads the marker of each path
    const json manifest = getBuildArtifactsJson(ds.db.get(), {"mixA/model.obj", "mixB/model.obj"});
    EXPECT_EQ(manifest[0]["build"]["awaitingFiles"], false);
    EXPECT_EQ(manifest[1]["build"]["awaitingFiles"], true);
    EXPECT_EQ(manifest[1]["build"]["missingDependencies"], json::array({"mixB/brighton.mtl"}));
    EXPECT_EQ(manifest[1]["build"]["state"], "pending");
}

// ---- FR-VWR-17 ------------------------------------------------------------------------------------

// The same splat at the root and in a folder with a georeference: two keys, so two build folders
TEST(BuildKey, AGeoreferencedCopyOfASplatHasItsOwnBuildFolder) {
    Dataset ds(TEST_NAME);
    const std::string hash(64, 'a');
    insertEntry(ds.db.get(), "train.ply", hash, EntryType::GaussianSplat);
    insertEntry(ds.db.get(), "geo1/train.ply", hash, EntryType::GaussianSplat);
    ds.setGeoref("geo1/train.ply");

    EXPECT_EQ(ds.keyOf("train.ply"), hash);
    EXPECT_EQ(ds.keyOf("geo1/train.ply"), hash + "-" + kEnuDigest);

    // Only the root copy is built
    const fs::path build = fs::absolute(ds.buildDir());
    fs::create_directories(build / hash / "gsplat");
    std::ofstream(build / hash / "gsplat" / GsplatRadFileName) << "rad";

    EXPECT_TRUE(isBuildComplete(ds.db.get(), "train.ply"));
    EXPECT_FALSE(isBuildComplete(ds.db.get(), "geo1/train.ply")) << "the root build is not the build of geo1";
    const auto status = getBuildStatus(ds.db.get(), {"train.ply", "geo1/train.ply"});
    EXPECT_TRUE(status[0].complete);
    EXPECT_FALSE(status[1].complete);

    const json manifest = getBuildArtifactsJson(ds.db.get(), {"train.ply", "geo1/train.ply"});
    EXPECT_EQ(manifest[0]["build"]["key"], hash);
    EXPECT_EQ(manifest[0]["build"]["hash"], hash);
    EXPECT_EQ(manifest[0]["build"]["root"], (build / hash).string());
    EXPECT_EQ(manifest[0]["build"]["state"], "complete");
    EXPECT_EQ(manifest[1]["build"]["key"], hash + "-" + kEnuDigest);
    EXPECT_EQ(manifest[1]["build"]["hash"], hash);
    EXPECT_EQ(manifest[1]["build"]["root"], (build / (hash + "-" + kEnuDigest)).string());
    EXPECT_EQ(manifest[1]["build"]["state"], "none");
    EXPECT_TRUE(manifest[1]["build"]["artifacts"].empty());

    // Once geo1 is built under its key, it is complete and lists its own artifacts
    const std::string key = hash + "-" + kEnuDigest;
    fs::create_directories(build / key / "gsplat");
    std::ofstream(build / key / "gsplat" / GsplatRadFileName) << "rad of geo1";
    EXPECT_TRUE(isBuildComplete(ds.db.get(), "geo1/train.ply"));
    EXPECT_EQ(resolveBuildFile(ds.db.get(), key, "gsplat/model.rad").path, build / key / "gsplat" / "model.rad");
    EXPECT_EQ(resolveBuildFile(ds.db.get(), hash, "gsplat/model.rad").path, build / hash / "gsplat" / "model.rad");

    // The key names a build of the georeferenced copy only
    EXPECT_THROW(resolveBuildFile(ds.db.get(), hash + "-" + std::string(16, '0'), "gsplat/model.rad"),
                 EntryNotFoundException);
    EXPECT_THROW(resolveBuildFile(ds.db.get(), hash + "-xyz", "gsplat/model.rad"), InvalidArgsException);

    // A sidecar-free type keeps the folder of its hash
    insertEntry(ds.db.get(), "ortho.tif", std::string(64, 'b'), EntryType::GeoRaster);
    EXPECT_EQ(ds.keyOf("ortho.tif"), std::string(64, 'b'));
}

// The folders and the markers a cleanup keeps are those of the keys and paths the index has
TEST(BuildKey, CleanupKeepsTheFoldersAndMarkersOfTheKeysTheIndexHas) {
    Dataset ds(TEST_NAME);
    ds.write("a.txt", "x");
    ds.add({"a.txt"});
    const std::string hash(64, 'a');
    insertEntry(ds.db.get(), "train.ply", hash, EntryType::GaussianSplat);
    insertEntry(ds.db.get(), "geo1/train.ply", hash, EntryType::GaussianSplat);
    ds.setGeoref("geo1/train.ply");
    // The cleanup first removes entries whose file is gone: give them files
    ds.write("train.ply", "x");
    ds.write("geo1/train.ply", "x");

    const fs::path build = ds.buildDir();
    const std::string key = hash + "-" + kEnuDigest;
    const std::string stale = hash + "-" + std::string(16, '1');
    for (const auto& folder : {hash, key, stale})
        fs::create_directories(build / folder / "gsplat");
    const auto marker = [&](const std::string& k, const std::string& path) {
        std::ofstream(build / pendingMarkerName(k, path)) << "0\nx.mtl\n";
        return build / pendingMarkerName(k, path);
    };
    const fs::path keep = marker(key, "geo1/train.ply");
    const fs::path other = marker(key, "elsewhere.ply");           // no entry has that path
    const fs::path oldKey = marker(stale, "geo1/train.ply");       // the key of the path is another one
    const fs::path keepRoot = marker(hash, "train.ply");

    cleanupBuild(ds.db.get());

    EXPECT_TRUE(fs::exists(build / hash)) << "the root copy has this key";
    EXPECT_TRUE(fs::exists(build / key));
    EXPECT_FALSE(fs::exists(build / stale)) << "no entry has this key";
    EXPECT_TRUE(fs::exists(keep));
    EXPECT_TRUE(fs::exists(keepRoot));
    EXPECT_FALSE(fs::exists(other));
    EXPECT_FALSE(fs::exists(oldKey));
}

// ---- Markers of the layout before the build key -----------------------------------------------------

// A marker "<hash>.pending" was the marker of every entry of that content: the rescan gives each its own,
// released (its names were those of one of the entries), so the next retry of each checks its own files
TEST(BuildKey, TheRescanMovesALegacyMarkerToTheMarkersOfItsEntries) {
    Dataset ds(TEST_NAME);
    for (const char* rel : {"mixA/model.obj", "mixB/model.obj"})
        ds.write(rel, "mtllib brighton.mtl\n" + kObjBody);
    ds.add({"mixA/model.obj", "mixB/model.obj"});
    const std::string hash = hashOf(ds.db.get(), "mixA/model.obj");
    fs::create_directories(ds.buildDir());
    std::ofstream(ds.buildDir() / (hash + ".pending")) << "1700000000\nbrighton.mtl\n";

    rescanIndex(ds.db.get(), {}, {}, true);

    EXPECT_EQ(ds.markerFiles(),
              (std::vector<std::string>{hash + ".2b7bc1b74ec754ab.pending", hash + ".46b6e6e1b32663ef.pending"}));
    const auto pending = ds.pendingByPath();
    ASSERT_EQ(pending.size(), 2u);
    for (const char* rel : {"mixA/model.obj", "mixB/model.obj"}) {
        EXPECT_EQ(pending.at(rel).lastAttempt, 0) << rel;
        EXPECT_TRUE(pending.at(rel).missingDependencies.empty()) << rel;
        EXPECT_FALSE(pending.at(rel).awaitingFiles) << rel;
    }
}

// A legacy marker holds the dataset-relative names of the entry built last ("mixA/brighton.mtl"). After the
// move, the twin in mixB waits for its own file: the .mtl uploaded to mixB releases it, not the one of mixA
TEST(BuildKey, ATwinOfALegacyMarkerWaitsForItsOwnFileAfterTheMove) {
    Dataset ds(TEST_NAME);
    for (const char* rel : {"mixA/model.obj", "mixB/model.obj"})
        ds.write(rel, "mtllib brighton.mtl\n" + kObjBody);
    ds.add({"mixA/model.obj", "mixB/model.obj"});
    const std::string hash = hashOf(ds.db.get(), "mixA/model.obj");
    fs::create_directories(ds.buildDir());
    std::ofstream(ds.buildDir() / (hash + ".pending")) << "1700000000\nmixA/brighton.mtl\n";

    // The move, then the retry of each entry (both still miss their .mtl)
    buildPending(ds.db.get(), "", false);
    EXPECT_FALSE(fs::exists(ds.buildDir() / (hash + ".pending")));
    auto pending = ds.pendingByPath();
    ASSERT_EQ(pending.size(), 2u);
    EXPECT_EQ(pending.at("mixA/model.obj").missingDependencies, std::vector<std::string>{"mixA/brighton.mtl"});
    EXPECT_EQ(pending.at("mixB/model.obj").missingDependencies, std::vector<std::string>{"mixB/brighton.mtl"});

    ds.write("mixB/brighton.mtl", kMtl);
    ds.add({"mixB/brighton.mtl"});
    pending = ds.pendingByPath();
    ASSERT_EQ(pending.size(), 2u);
    EXPECT_FALSE(pending.at("mixB/model.obj").awaitingFiles) << "its own .mtl was uploaded";
    EXPECT_TRUE(pending.at("mixA/model.obj").awaitingFiles);
    EXPECT_EQ(pending.at("mixA/model.obj").missingDependencies, std::vector<std::string>{"mixA/brighton.mtl"});
}

// ---- Moving an entry ------------------------------------------------------------------------------------

// A pending model moved to another folder (as a host moves it: the file on disk, then the index) keeps
// its marker, renamed to its new path and released, so the retry names the files of the new place; the
// marker of the old path is gone and a cleanup keeps the new one
TEST(BuildKey, AMovedPendingModelKeepsItsMarker) {
    Dataset ds(TEST_NAME);
    ds.write("a/model.obj", "mtllib brighton.mtl\n" + kObjBody);
    ds.add({"a/model.obj"});
    const std::string hash = hashOf(ds.db.get(), "a/model.obj");
    EXPECT_THROW(build(ds.db.get(), "a/model.obj", "", false), BuildDepMissingException);
    ASSERT_EQ(ds.markerFiles(), std::vector<std::string>{hash + ".d1f4e378cf00e72f.pending"}) << "a/model.obj";

    fs::create_directories(ds.path("b"));
    fs::rename(ds.path("a/model.obj"), ds.path("b/model.obj"));
    moveEntry(ds.db.get(), "a/model.obj", "b/model.obj");

    EXPECT_EQ(ds.markerFiles(), std::vector<std::string>{hash + ".e3dd06262261fc73.pending"}) << "b/model.obj";
    auto pending = ds.pendingByPath();
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_FALSE(pending.at("b/model.obj").awaitingFiles) << "the names of a/ are not those of b/";
    cleanupBuild(ds.db.get());
    EXPECT_EQ(ds.markerFiles(), std::vector<std::string>{hash + ".e3dd06262261fc73.pending"});

    buildPending(ds.db.get(), "", false);
    pending = ds.pendingByPath();
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending.at("b/model.obj").missingDependencies, std::vector<std::string>{"b/brighton.mtl"});

    ds.write("b/brighton.mtl", kMtl);
    ds.add({"b/brighton.mtl"});
    pending = ds.pendingByPath();
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_FALSE(pending.at("b/model.obj").awaitingFiles);
    EXPECT_FALSE(fs::exists(ds.buildDir() / (hash + ".d1f4e378cf00e72f.pending")));
}

// A moved folder takes the markers of the pending entries in it along
TEST(BuildKey, AMovedFolderTakesTheMarkersOfItsPendingEntries) {
    Dataset ds(TEST_NAME);
    ds.write("c/model.obj", "mtllib brighton.mtl\n" + kObjBody);
    ds.add({"c"});
    const std::string hash = hashOf(ds.db.get(), "c/model.obj");
    EXPECT_THROW(build(ds.db.get(), "c/model.obj", "", false), BuildDepMissingException);
    ASSERT_EQ(ds.markerFiles(), std::vector<std::string>{hash + ".408e59a68958b31d.pending"}) << "c/model.obj";

    fs::rename(ds.path("c"), ds.path("d"));
    moveEntry(ds.db.get(), "c", "d");

    EXPECT_EQ(ds.markerFiles(), std::vector<std::string>{hash + ".9cbfb27efd12bd63.pending"}) << "d/model.obj";
    cleanupBuild(ds.db.get());
    EXPECT_EQ(ds.markerFiles(), std::vector<std::string>{hash + ".9cbfb27efd12bd63.pending"});

    buildPending(ds.db.get(), "", false);
    auto pending = ds.pendingByPath();
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending.at("d/model.obj").missingDependencies, std::vector<std::string>{"d/brighton.mtl"});

    ds.write("d/brighton.mtl", kMtl);
    ds.add({"d/brighton.mtl"});
    pending = ds.pendingByPath();
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_FALSE(pending.at("d/model.obj").awaitingFiles);
}

// ---- The rescan of D-020 ------------------------------------------------------------------------------

struct KeyReport {
    std::string key;
    bool changed = false;
};

std::map<std::string, KeyReport> rescanKeys(Database* db) {
    std::map<std::string, KeyReport> reports;
    rescanIndex(
        db, {}, {}, true, nullptr,
        [&](const Entry& e, const std::string& key, bool changed) { reports[e.path] = {key, changed}; });
    return reports;
}

// Only the entries whose key changed are reported: the place-independent ones and the models or
// splats without a georeference keep their builds
TEST(BuildKey, TheRescanReportsOnlyTheEntriesWhoseKeyChanged) {
    Dataset ds(TEST_NAME);
    ds.write("plain.obj", kObjBody);
    ds.write("sub/model.obj", kObjBody);
    ds.write("a.txt", "x");
    ds.add({"plain.obj", "sub/model.obj", "a.txt"});
    const std::string hash = hashOf(ds.db.get(), "plain.obj");
    ASSERT_EQ(hash, hashOf(ds.db.get(), "sub/model.obj"));

    auto reports = rescanKeys(ds.db.get());
    ASSERT_EQ(reports.size(), 3u);
    for (const auto& [path, report] : reports)
        EXPECT_FALSE(report.changed) << path;
    EXPECT_EQ(reports.at("sub/model.obj").key, hash);

    // A georef.json next to sub/model.obj (as an upload after the model): its key moves
    ds.write("sub/georef.json", R"({"latitude": 45.0, "longitude": 9.0})");
    reports = rescanKeys(ds.db.get());
    EXPECT_TRUE(reports.at("sub/model.obj").changed);
    EXPECT_EQ(reports.at("sub/model.obj").key, hash + "-" + kEnuDigest);
    EXPECT_FALSE(reports.at("plain.obj").changed) << "the same file in another folder is not touched";
    EXPECT_EQ(reports.at("plain.obj").key, hash);
    EXPECT_FALSE(reports.at("a.txt").changed);
    EXPECT_EQ(ds.keyOf("sub/model.obj"), hash + "-" + kEnuDigest);

    // Not built under its key, and no build of its hash to move (a deferred build, or one that keeps
    // failing): the next rescans do not report it again
    reports = rescanKeys(ds.db.get());
    EXPECT_FALSE(reports.at("sub/model.obj").changed);
    reports = rescanKeys(ds.db.get());
    EXPECT_FALSE(reports.at("sub/model.obj").changed);

    // A build of its hash (a 2.x build) and none of its key: the move is reported until it is built
    fs::create_directories(ds.buildDir() / hash / "bm3d");
    reports = rescanKeys(ds.db.get());
    EXPECT_TRUE(reports.at("sub/model.obj").changed);
    EXPECT_FALSE(reports.at("plain.obj").changed);
    reports = rescanKeys(ds.db.get());
    EXPECT_TRUE(reports.at("sub/model.obj").changed);
    fs::create_directories(ds.buildDir() / (hash + "-" + kEnuDigest) / "bm3d");
    reports = rescanKeys(ds.db.get());
    for (const auto& [path, report] : reports)
        EXPECT_FALSE(report.changed) << path;

    // A changed georeference is a changed key
    ds.write("sub/georef.json", R"({"latitude": 46.0, "longitude": 9.0})");
    reports = rescanKeys(ds.db.get());
    EXPECT_TRUE(reports.at("sub/model.obj").changed);
    EXPECT_NE(reports.at("sub/model.obj").key, hash + "-" + kEnuDigest);
    EXPECT_EQ(reports.at("sub/model.obj").key, ds.keyOf("sub/model.obj"));
    EXPECT_FALSE(reports.at("plain.obj").changed);
}

// A georeferenced model that has no build at all (deferred, or failing for good) has nothing to move: no
// rescan reports it
TEST(BuildKey, AGeoreferencedModelWithoutABuildIsNotReported) {
    Dataset ds(TEST_NAME);
    ds.write("sub/model.obj", "mtllib missing.mtl\n" + kObjBody);
    ds.write("sub/georef.json", R"({"latitude": 45.0, "longitude": 9.0})");
    ds.add({"sub/model.obj"});
    const std::string hash = hashOf(ds.db.get(), "sub/model.obj");
    ASSERT_EQ(ds.keyOf("sub/model.obj"), hash + "-" + kEnuDigest);
    ASSERT_FALSE(fs::exists(ds.buildDir() / hash));

    for (int i = 0; i < 3; i++) {
        const auto reports = rescanKeys(ds.db.get());
        EXPECT_FALSE(reports.at("sub/model.obj").changed) << "rescan " << i + 1;
        EXPECT_EQ(reports.at("sub/model.obj").key, hash + "-" + kEnuDigest);
    }
}

// A model whose folder is named by its hash (a 2.x build) is moved by the first rescan, which reports it
TEST(BuildKey, TheFirstRescanAfterTheUpgradeReportsTheGeoreferencedModelsOnly) {
    Dataset ds(TEST_NAME);
    ds.write("sub/model.obj", kObjBody);
    ds.write("sub/georef.json", R"({"latitude": 45.0, "longitude": 9.0})");
    ds.write("other/model.obj", kObjBody + "# another model\n");
    ds.add({"sub/model.obj", "other/model.obj"});
    // The rows of a 2.x index have the same properties: the georef is there, the folders are by hash
    for (const char* rel : {"sub/model.obj", "other/model.obj"}) {
        const std::string hash = hashOf(ds.db.get(), rel);
        fs::create_directories(ds.buildDir() / hash / "bm3d");
        std::ofstream(ds.buildDir() / hash / "bm3d" / "model.bm3d") << "bm3d";
    }
    ASSERT_TRUE(isBuildComplete(ds.db.get(), "other/model.obj"));
    EXPECT_FALSE(isBuildComplete(ds.db.get(), "sub/model.obj")) << "its build is not under its key yet";

    const auto reports = rescanKeys(ds.db.get());
    EXPECT_TRUE(reports.at("sub/model.obj").changed);
    EXPECT_FALSE(reports.at("other/model.obj").changed);
    EXPECT_TRUE(isBuildComplete(ds.db.get(), "other/model.obj"));
}

}  // namespace
