/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// XI-04: the build manifest (build_manifest.h). The build state and the existing artifacts of
// entries come from the EntryHandler registry; reading them is side-effect free and costs a
// bounded number of filesystem queries (counted through the BuildProbe seam); build files are
// resolved inside an artifact only; a build copies with its pending marker.

#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "build.h"
#include "build_internal.h"
#include "build_layout.h"
#include "build_manifest.h"
#include "buildlock.h"
#include "contract_fixtures.h"
#include "dbops.h"
#include "entry_handler.h"
#include "exceptions.h"
#include "gtest/gtest.h"
#include "mio.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace {

using namespace ddb;

const std::string H_COG(64, 'a');      // ortho.tif: complete COG
const std::string H_DSM(64, 'b');      // dsm.tif: never built
const std::string H_COPC(64, 'c');     // cloud.laz: COPC, and an EPT leftover
const std::string H_ROADS(64, 'd');    // roads.shp: GeoPackage + MVT
const std::string H_EPT(64, '3');      // old.laz: legacy EPT only
const std::string H_CAD(64, '4');      // plan.dxf: GeoPackage + local.json (no CRS)
const std::string H_PENDING(64, '5');  // pending.shp: deferred, a dependency missing
const std::string H_BUSY(64, '6');     // busy.tif: a live build holds the lock
const std::string H_STALE(64, '7');    // stale.tif: complete, a crashed build's lock file
const std::string H_SPLAT(64, '8');    // splat.spz: RAD + bounds
const std::string H_MODEL(64, '9');    // model.obj: Nexus + 3D Tiles
const std::string H_TILES(64, 'f');    // city.3tz: 3D Tiles

const std::vector<std::string> kPaths = {"ortho.tif",  "dsm.tif",   "cloud.laz",  "old.laz",   "roads.shp",
                                         "roads.dbf",  "plan.dxf",  "pending.shp", "busy.tif", "stale.tif",
                                         "splat.spz",  "model.obj", "city.3tz",   "photo.jpg", "orphan.dbf",
                                         "dir",        "missing.tif"};

struct ManifestFixture : IndexFixture {
    fs::path build;

    explicit ManifestFixture(const std::string& name) : IndexFixture(name), build(fs::absolute(db->buildDirectory())) {
        insertEntry(db.get(), "ortho.tif", H_COG, GeoRaster);
        insertEntry(db.get(), "dsm.tif", H_DSM, GeoRaster);
        insertEntry(db.get(), "cloud.laz", H_COPC, PointCloud);
        insertEntry(db.get(), "old.laz", H_EPT, PointCloud);
        insertEntry(db.get(), "roads.shp", H_ROADS, Vector);
        insertEntry(db.get(), "roads.dbf", std::string(64, 'e'), Generic);
        insertEntry(db.get(), "plan.dxf", H_CAD, Vector);
        insertEntry(db.get(), "pending.shp", H_PENDING, Vector);
        insertEntry(db.get(), "busy.tif", H_BUSY, GeoRaster);
        insertEntry(db.get(), "stale.tif", H_STALE, GeoRaster);
        insertEntry(db.get(), "splat.spz", H_SPLAT, GaussianSplat);
        insertEntry(db.get(), "model.obj", H_MODEL, Model);
        insertEntry(db.get(), "city.3tz", H_TILES, Tiles3D);
        insertEntry(db.get(), "photo.jpg", std::string(64, '1'), GeoImage);
        insertEntry(db.get(), "orphan.dbf", std::string(64, '2'), Generic);
        insertEntry(db.get(), "dir", "", Directory);

        writeFile(build / H_COG / "cog" / "cog.tif", "x");
        writeFile(build / H_COPC / "copc" / "cloud.copc.laz", "x");
        writeFile(build / H_COPC / "ept" / "ept.json", "{}");
        writeFile(build / H_EPT / "ept" / "ept.json", "{}");
        writeFile(build / H_ROADS / "vec" / "source.gpkg", "x");
        writeFile(build / H_ROADS / "mvt" / "metadata.json", "{}");
        writeFile(build / H_ROADS / "mvt" / "0" / "0" / "0.pbf", "x");
        writeFile(build / H_CAD / "vec" / "source.gpkg", "x");
        writeFile(build / H_CAD / "vec" / "local.json", "{}");
        writeFile(build / (H_PENDING + ".pending"), "1700000000\nsub/pending.shx\nsub/pending.dbf\n");
        fs::create_directories(build / H_BUSY);
        writeFile(build / H_STALE / "cog" / "cog.tif", "x");
        writeFile(build / H_STALE / "cog.building", "12345");  // no live holder
        writeFile(build / H_SPLAT / "gsplat" / "model.rad", "x");
        writeFile(build / H_SPLAT / "gsplat" / "bounds.json", "{}");
        writeFile(build / H_MODEL / "nxs" / "model.nxz", "x");
        writeFile(build / H_MODEL / "3dtiles" / "tileset.json", "{}");
        writeFile(build / H_TILES / "3dtiles" / "tileset.json", "{}");
    }

    std::map<std::string, EntryBuild> readAll(const BuildProbe& probe = filesystemBuildProbe()) {
        BuildStateReader reader(db.get(), probe);
        std::map<std::string, EntryBuild> builds;
        for (const auto& path : kPaths) {
            Entry e;
            if (reader.lookup(path, e))
                builds[path] = reader.read(e);
        }
        return builds;
    }
};

std::vector<std::string> roles(const EntryBuild& b) {
    std::vector<std::string> r;
    for (const auto& a : b.artifacts)
        r.push_back(a.role);
    return r;
}

// Counts every filesystem query of a build state read, by kind
class CountingProbe : public BuildProbe {
  public:
    mutable std::map<std::string, int> calls;

    int total() const {
        int n = 0;
        for (const auto& [kind, count] : calls)
            n += count;
        return n;
    }

    std::optional<std::vector<DirectoryName>> list(const fs::path& dir) const override {
        calls["list"]++;
        return BuildProbe::list(dir);
    }
    bool isFile(const fs::path& path) const override {
        calls["isFile"]++;
        return BuildProbe::isFile(path);
    }
    std::optional<PendingMarker> readPendingMarker(const fs::path& path) const override {
        calls["readPendingMarker"]++;
        return BuildProbe::readPendingMarker(path);
    }
    bool isLockHeld(const fs::path& outputFolder) const override {
        calls["isLockHeld"]++;
        return BuildProbe::isLockHeld(outputFolder);
    }
    bool isComplete(const EntryHandler& handler, const fs::path& root) const override {
        calls["isComplete"]++;
        return BuildProbe::isComplete(handler, root);
    }
};

TEST(BuildManifest, StatesAndExistingArtifacts) {
    ManifestFixture fx(TEST_NAME);
    BuildLock liveBuild((fx.build / H_BUSY / "cog").string());
    auto b = fx.readAll();

    EXPECT_EQ(b["ortho.tif"].state, BuildState::Complete);
    EXPECT_EQ(roles(b["ortho.tif"]), (std::vector<std::string>{"raster.cog"}));
    EXPECT_EQ(b["ortho.tif"].source, "ortho.tif");
    EXPECT_EQ(b["ortho.tif"].hash, H_COG);
    EXPECT_EQ(b["ortho.tif"].root, fx.build / H_COG);

    EXPECT_TRUE(b["dsm.tif"].buildable);
    EXPECT_EQ(b["dsm.tif"].state, BuildState::None);
    EXPECT_TRUE(b["dsm.tif"].artifacts.empty());
    EXPECT_FALSE(b["dsm.tif"].lastAttempt.has_value());

    // The legacy EPT is served only while there is no COPC (Registry's fallback, now in libddb)
    EXPECT_EQ(roles(b["cloud.laz"]), (std::vector<std::string>{"pointcloud.copc"}));
    EXPECT_EQ(roles(b["old.laz"]), (std::vector<std::string>{"pointcloud.ept"}));
    EXPECT_TRUE(b["old.laz"].artifacts[0].legacy);
    EXPECT_EQ(b["old.laz"].state, BuildState::None) << "an EPT-only point cloud is rebuilt to COPC";

    EXPECT_EQ(b["roads.shp"].state, BuildState::Complete);
    EXPECT_EQ(roles(b["roads.shp"]), (std::vector<std::string>{"vector.gpkg", "vector.mvt"}));
    EXPECT_EQ(roles(b["plan.dxf"]), (std::vector<std::string>{"vector.gpkg", "vector.local"}));
    EXPECT_EQ(b["plan.dxf"].state, BuildState::Complete);

    // A sidecar reports its main file's build
    EXPECT_TRUE(b["roads.dbf"].buildable);
    EXPECT_EQ(b["roads.dbf"].source, "roads.shp");
    EXPECT_EQ(b["roads.dbf"].hash, H_ROADS);
    EXPECT_EQ(roles(b["roads.dbf"]), roles(b["roads.shp"]));

    EXPECT_EQ(b["pending.shp"].state, BuildState::Pending);
    EXPECT_EQ(b["pending.shp"].missingDependencies,
              (std::vector<std::string>{"sub/pending.shx", "sub/pending.dbf"}));
    EXPECT_EQ(b["pending.shp"].lastAttempt, std::optional<time_t>(1700000000));

    EXPECT_EQ(b["busy.tif"].state, BuildState::Building);
    EXPECT_EQ(b["stale.tif"].state, BuildState::Complete) << "a lock file without a live holder is no build";

    EXPECT_EQ(roles(b["splat.spz"]), (std::vector<std::string>{"gsplat.lod", "gsplat.bounds"}));
    EXPECT_EQ(b["splat.spz"].state, BuildState::Complete);
    EXPECT_EQ(roles(b["model.obj"]), (std::vector<std::string>{"model.nexus", "model.3dtiles"}));
    EXPECT_EQ(roles(b["city.3tz"]), (std::vector<std::string>{"model.3dtiles"}));

    for (const auto* path : {"photo.jpg", "orphan.dbf", "dir"}) {
        SCOPED_TRACE(path);
        EXPECT_FALSE(b[path].buildable);
        EXPECT_EQ(b[path].state, BuildState::None);
        EXPECT_TRUE(b[path].hash.empty());
        EXPECT_TRUE(b[path].root.empty());
        EXPECT_TRUE(b[path].artifacts.empty());
    }
}

// "buildable" and "complete" agree with getBuildStatus(), the rules of DDBIsBuildable and
// DDBIsBuildComplete, for every entry that is neither building nor pending
TEST(BuildManifest, AgreesWithGetBuildStatus) {
    ManifestFixture fx(TEST_NAME);
    auto b = fx.readAll();
    const auto statuses = getBuildStatus(fx.db.get(), kPaths);
    for (const auto& s : statuses) {
        SCOPED_TRACE(s.path);
        if (!s.exists) {
            EXPECT_EQ(b.count(s.path), 0u);
            continue;
        }
        EXPECT_EQ(b[s.path].buildable, s.buildable);
        if (b[s.path].state != BuildState::Pending && b[s.path].state != BuildState::Building) {
            EXPECT_EQ(b[s.path].state == BuildState::Complete, s.complete);
        }
    }
}

// The probe budget (P3-6): no filesystem query for an entry that is not buildable; for a
// buildable one, the build root listing and the pending marker read, a lock probe only when a
// lock file exists, one query per existing artifact folder, and at most the completeness check
// getBuildStatus() makes (none for a building or pending entry, or a missing build root)
TEST(BuildManifest, ProbeBudget) {
    ManifestFixture fx(TEST_NAME);
    BuildLock liveBuild((fx.build / H_BUSY / "cog").string());
    BuildStateReader reader(fx.db.get(), filesystemBuildProbe());

    // The artifact folders of each entry's build that exist (file artifacts: their folder;
    // directory artifacts: their index)
    const std::map<std::string, int> existingFolders = {
        {"ortho.tif", 1}, {"cloud.laz", 1}, {"old.laz", 1}, {"roads.shp", 2}, {"roads.dbf", 2},
        {"plan.dxf", 1},  {"stale.tif", 1}, {"splat.spz", 1}, {"model.obj", 2}, {"city.3tz", 1}};
    const std::set<std::string> lockFiles = {"busy.tif", "stale.tif"};

    for (const auto& path : kPaths) {
        SCOPED_TRACE(path);
        Entry e;
        if (!reader.lookup(path, e))
            continue;
        CountingProbe probe;
        BuildStateReader counted(fx.db.get(), probe);
        const EntryBuild b = counted.read(e);

        if (!b.buildable) {
            EXPECT_EQ(probe.total(), 0) << "a non-buildable entry costs no filesystem query";
            continue;
        }
        const auto folders = existingFolders.count(path) ? existingFolders.at(path) : 0;
        const int lock = lockFiles.count(path) ? 1 : 0;
        EXPECT_EQ(probe.calls["readPendingMarker"], 1);
        EXPECT_EQ(probe.calls["isLockHeld"], lock) << "the lock is probed only when its file exists";
        EXPECT_LE(probe.calls["isComplete"], 1) << "at most getBuildStatus()'s completeness check";
        if (b.state == BuildState::Building || b.state == BuildState::Pending) {
            EXPECT_EQ(probe.calls["isComplete"], 0);
        }
        EXPECT_LE(probe.total() - probe.calls["isComplete"], 2 + folders + lock);
    }

    // A never-built entry: its build root listing and its marker read, nothing else
    Entry dsm;
    ASSERT_TRUE(reader.lookup("dsm.tif", dsm));
    CountingProbe probe;
    BuildStateReader counted(fx.db.get(), probe);
    counted.read(dsm);
    EXPECT_EQ(probe.total(), 2);
}

// Reading build states changes nothing on disk. DDBIsBuildActive removes a stale lock file
// (BuildLock::removeIfStale); the manifest only asks (BuildLock::isHeld).
TEST(BuildManifest, ReadingIsSideEffectFree) {
    ManifestFixture fx(TEST_NAME);
    const fs::path staleLock = fx.build / H_STALE / "cog.building";
    const auto before = getEntries(fx.build);

    const json listed = getBuildArtifactsJson(fx.db.get(), kPaths);
    EntryQuery everything;
    listEntriesJson(fx.db.get(), everything, true);

    EXPECT_TRUE(fs::exists(staleLock)) << "the manifest reclaimed a stale lock";
    EXPECT_EQ(getEntries(fx.build).size(), before.size());
    EXPECT_FALSE(fs::exists(fx.build / H_DSM)) << "a build root was created";
    EXPECT_EQ(listed.size(), kPaths.size());

    // The old probe, for contrast: it removes the leftover
    EXPECT_FALSE(isBuildActive(fx.db.get(), "stale.tif"));
    EXPECT_FALSE(fs::exists(staleLock));
}

TEST(BuildManifest, Json) {
    ManifestFixture fx(TEST_NAME);
    const json out = getBuildArtifactsJson(fx.db.get(), {"ortho.tif", "pending.shp", "dir", "missing.tif"});
    ASSERT_EQ(out.size(), 4u);

    EXPECT_EQ(out[0]["path"], "ortho.tif");
    EXPECT_EQ(out[0]["exists"], true);
    EXPECT_EQ(out[0]["hash"], H_COG);
    EXPECT_EQ(out[0]["type"], static_cast<int>(GeoRaster));
    const json& cog = out[0]["build"];
    EXPECT_EQ(cog["state"], "complete");
    EXPECT_EQ(cog["root"], io::toUtf8(fx.build / H_COG));
    EXPECT_TRUE(cog["lastAttempt"].is_null());
    EXPECT_EQ(cog["artifacts"],
              json::parse(R"([{"role": "raster.cog", "path": "cog/cog.tif", "kind": "file",
                  "mediaType": "image/tiff; application=geotiff; profile=cloud-optimized",
                  "downloadName": "cog.tif"}])"));

    EXPECT_EQ(out[1]["build"]["state"], "pending");
    EXPECT_EQ(out[1]["build"]["lastAttempt"], 1700000000);

    EXPECT_TRUE(out[2]["hash"].is_null());
    EXPECT_EQ(out[2]["build"], json::parse(R"({"buildable": false, "source": null, "hash": null, "root": null,
        "state": "none", "missingDependencies": [], "lastAttempt": null, "artifacts": []})"));

    EXPECT_EQ(out[3], json::parse(R"({"path": "missing.tif", "exists": false})"));
}

// DDBListEntriesEx without builds is DDBListEntries, byte for byte; with builds each entry gets
// the object DDBGetBuildArtifacts reports for it
TEST(BuildManifest, ListEntriesWithBuild) {
    ManifestFixture fx(TEST_NAME);
    EntryQuery query;
    query.withDetails = false;

    std::ostringstream plain;
    writeEntries(plain, listEntries(fx.db.get(), query), "json");
    EXPECT_EQ(listEntriesJson(fx.db.get(), query, false), plain.str());

    const json listed = json::parse(listEntriesJson(fx.db.get(), query, true));
    const json direct = getBuildArtifactsJson(fx.db.get(), kPaths);
    ASSERT_EQ(listed.size(), kPaths.size() - 1);  // missing.tif
    for (const auto& item : listed) {
        SCOPED_TRACE(item["path"].get<std::string>());
        const auto it = std::find_if(direct.begin(), direct.end(),
                                     [&](const json& d) { return d["path"] == item["path"]; });
        ASSERT_NE(it, direct.end());
        EXPECT_EQ(item["build"], (*it)["build"]);
    }
}

TEST(BuildManifest, EntryTypes) {
    const json types = entryTypesJson();
    std::map<std::string, json> byName;
    int previous = -1;
    for (const auto& t : types) {
        EXPECT_GT(t["id"].get<int>(), previous) << "sorted by id";
        previous = t["id"].get<int>();
        byName[t["name"].get<std::string>()] = t;
        EXPECT_EQ(t["buildable"], entryHandler(static_cast<EntryType>(previous)).buildable());
    }
    EXPECT_EQ(byName["GeoRaster"]["roles"], json::parse(R"(["raster.cog"])"));
    EXPECT_EQ(byName["GeoRaster"]["thumbnail"], "file");
    EXPECT_EQ(byName["PointCloud"]["roles"], json::parse(R"(["pointcloud.copc", "pointcloud.ept"])"));
    EXPECT_EQ(byName["PointCloud"]["thumbnail"], "build");
    EXPECT_EQ(byName["Vector"]["sidecarExtensions"], json::parse(R"(["cpg", "dbf", "shx", "prj"])"));
    EXPECT_EQ(byName["Panorama"]["thumbnail"], "none");
    EXPECT_EQ(byName["Generic"]["buildable"], false);
    EXPECT_TRUE(byName["Generic"]["roles"].empty());
}

// Real builds (GDAL, in process) produce what the handlers declare: every listed artifact
// resolves, a directory through its index
TEST(BuildManifest, RealBuildsMatchTheManifest) {
    EnvVarGuard noDiskFloor("DDB_MIN_FREE_DISK_MB", "0");
    IndexFixture fx(TEST_NAME);
    for (const auto* name : {"ortho.tif", "layer.geojson"})
        contract::copyInputs(contract::inputsDir() / name, fx.root / name);
    addToIndex(fx.db.get(), {(fx.root / "ortho.tif").string(), (fx.root / "layer.geojson").string()});
    buildAll(fx.db.get(), "", false);

    const json out = getBuildArtifactsJson(fx.db.get(), {"ortho.tif", "layer.geojson"});
    const std::vector<std::vector<std::string>> expected = {{"raster.cog"}, {"vector.gpkg", "vector.mvt"}};
    for (size_t i = 0; i < out.size(); i++) {
        SCOPED_TRACE(out[i]["path"].get<std::string>());
        const json& build = out[i]["build"];
        EXPECT_EQ(build["state"], "complete");
        std::vector<std::string> found;
        for (const auto& a : build["artifacts"]) {
            found.push_back(a["role"].get<std::string>());
            const std::string file = a["kind"] == "dir" ? a["path"].get<std::string>() + "/" + a["index"].get<std::string>()
                                                        : a["path"].get<std::string>();
            const fs::path resolved = resolveBuildFile(fx.db.get(), build["hash"].get<std::string>(), file);
            EXPECT_TRUE(fileExistsAndNonEmpty(resolved)) << file;
        }
        EXPECT_EQ(found, expected[i]);
    }
}

// ---- A new type reaches the manifest with no other change (DQ-08 acceptance) -------------------

constexpr auto ManifestFakeType = static_cast<EntryType>(31);

class ManifestFakeHandler : public EntryHandler {
  public:
    ManifestFakeHandler() : EntryHandler(ManifestFakeType, "ManifestFake") {}
    std::vector<std::string> extensions() const override { return {"ddbmanifestfake"}; }
    std::string subfolder() const override { return "fake"; }
    std::vector<BuildArtifact> artifacts() const override {
        return {{"fake.out", "fake/out.txt", ArtifactKind::File, "text/plain", "out.txt", "", false}};
    }
};

TEST(BuildManifest, FakeTypeAppearsInTheManifest) {
    ManifestFixture fx(TEST_NAME);
    const std::string hash(64, '0');
    {
        ScopedEntryHandler registration(std::make_unique<ManifestFakeHandler>());
        insertEntry(fx.db.get(), "a.ddbmanifestfake", hash, ManifestFakeType);
        writeFile(fx.build / hash / "fake" / "out.txt", "x");

        const json types = entryTypesJson();
        const auto fake = std::find_if(types.begin(), types.end(),
                                       [](const json& t) { return t["name"] == "ManifestFake"; });
        ASSERT_NE(fake, types.end());
        EXPECT_EQ((*fake)["roles"], json::parse(R"(["fake.out"])"));
        EXPECT_EQ((*fake)["buildable"], true);

        const json out = getBuildArtifactsJson(fx.db.get(), {"a.ddbmanifestfake"});
        EXPECT_EQ(out[0]["build"]["state"], "complete");
        EXPECT_EQ(out[0]["build"]["artifacts"][0]["role"], "fake.out");
        EXPECT_EQ(resolveBuildFile(fx.db.get(), hash, "fake/out.txt"), fx.build / hash / "fake" / "out.txt");
    }
    EXPECT_TRUE(getBuildArtifactsJson(fx.db.get(), {"a.ddbmanifestfake"})[0]["build"]["artifacts"].empty());
}

// ---- Build files ---------------------------------------------------------------------------------

TEST(BuildManifest, ResolveBuildFile) {
    ManifestFixture fx(TEST_NAME);
    EXPECT_EQ(resolveBuildFile(fx.db.get(), H_COG, "cog/cog.tif"), fx.build / H_COG / "cog" / "cog.tif");
    EXPECT_EQ(resolveBuildFile(fx.db.get(), H_ROADS, "mvt/0/0/0.pbf"),
              fx.build / H_ROADS / "mvt" / "0" / "0" / "0.pbf");
    EXPECT_EQ(resolveBuildFile(fx.db.get(), H_ROADS, "mvt\\metadata.json"),
              fx.build / H_ROADS / "mvt" / "metadata.json");
    EXPECT_EQ(resolveBuildFile(fx.db.get(), H_COPC, "ept/ept.json"), fx.build / H_COPC / "ept" / "ept.json")
        << "legacy artifacts are served";

    // Malformed: InvalidArgs (DDBERR_INVALIDARGS)
    for (const auto* path : {"../ortho.tif", "cog/../../x", "/etc/passwd", "C:\\x", "", "./", "cog/.ddb/x"})
        EXPECT_THROW(resolveBuildFile(fx.db.get(), H_COG, path), InvalidArgsException) << path;
    EXPECT_THROW(resolveBuildFile(fx.db.get(), "abc", "cog/cog.tif"), InvalidArgsException);

    // Not an artifact, or missing: NotFound (DDBERR_NOTFOUND)
    writeFile(fx.build / H_COG / "cog-temp-0123456789abcdef" / "cog.tif", "x");
    for (const auto* path : {"cog.building", "cog-temp-0123456789abcdef/cog.tif", "cog/other.tif", "cog", "mvt/x"})
        EXPECT_THROW(resolveBuildFile(fx.db.get(), H_COG, path), EntryNotFoundException) << path;
    EXPECT_THROW(resolveBuildFile(fx.db.get(), H_DSM, "cog/cog.tif"), EntryNotFoundException);
    EXPECT_THROW(resolveBuildFile(fx.db.get(), std::string(64, '0'), "cog/cog.tif"), EntryNotFoundException);
    EXPECT_THROW(resolveBuildFile(fx.db.get(), H_ROADS, "mvt"), EntryNotFoundException) << "a folder is no file";
}

#ifndef WIN32
// A link inside an artifact folder must not lead out of the build root
TEST(BuildManifest, ResolveBuildFileRejectsALinkOut) {
    ManifestFixture fx(TEST_NAME);
    const fs::path secret = writeFile(fx.root / "secret.txt", "secret");
    fs::create_symlink(secret, fx.build / H_ROADS / "mvt" / "leak.pbf");
    EXPECT_THROW(resolveBuildFile(fx.db.get(), H_ROADS, "mvt/leak.pbf"), EntryNotFoundException);
}
#endif

// ---- Copy ----------------------------------------------------------------------------------------

struct CopyFixture {
    ManifestFixture source;
    IndexFixture destination;
    fs::path destinationBuild;

    explicit CopyFixture(const std::string& name)
        : source(name), destination(name, "dst", false),
          destinationBuild(fs::absolute(destination.db->buildDirectory())) {}
};

TEST(BuildManifest, CopyBuildArtifacts) {
    EnvVarGuard noDiskFloor("DDB_MIN_FREE_DISK_MB", "0");
    CopyFixture fx(TEST_NAME);

    // The destination's old copy of a folder is replaced, its other files are left alone
    writeFile(fx.destinationBuild / H_ROADS / "mvt" / "old.pbf", "old");
    writeFile(fx.destinationBuild / H_ROADS / "unrelated.txt", "keep");
    const json out = copyBuildArtifacts(fx.source.db.get(), fx.destination.db.get(), H_ROADS);
    EXPECT_EQ(out["hash"], H_ROADS);
    EXPECT_EQ(out["pending"], false);
    ASSERT_EQ(out["artifacts"].size(), 2u);
    EXPECT_EQ(out["artifacts"][0]["role"], "vector.gpkg");
    EXPECT_EQ(out["artifacts"][1]["role"], "vector.mvt");
    EXPECT_TRUE(fs::exists(fx.destinationBuild / H_ROADS / "mvt" / "0" / "0" / "0.pbf"));
    EXPECT_FALSE(fs::exists(fx.destinationBuild / H_ROADS / "mvt" / "old.pbf"));
    EXPECT_TRUE(fs::exists(fx.destinationBuild / H_ROADS / "unrelated.txt"));
    for (const auto& e : fs::directory_iterator(fx.destinationBuild / H_ROADS)) {
        const auto name = e.path().filename().string();
        EXPECT_EQ(name.find(BuildLockSuffix), std::string::npos) << name << ": a lock was left";
        EXPECT_EQ(name.find(TempFolderInfix), std::string::npos) << name << ": a scratch folder was left";
    }

    // The pending marker travels with its timestamp and dependencies
    const json pending = copyBuildArtifacts(fx.source.db.get(), fx.destination.db.get(), H_PENDING);
    EXPECT_EQ(pending["pending"], true);
    EXPECT_TRUE(pending["artifacts"].empty());
    const auto marker = readPendingMarker(fx.destinationBuild / (H_PENDING + ".pending"));
    ASSERT_TRUE(marker.has_value());
    EXPECT_EQ(marker->lastAttempt, 1700000000);
    EXPECT_EQ(marker->missingDependencies, (std::vector<std::string>{"sub/pending.shx", "sub/pending.dbf"}));

    // Legacy folders are copied too; nothing built copies nothing
    EXPECT_EQ(copyBuildArtifacts(fx.source.db.get(), fx.destination.db.get(), H_EPT)["artifacts"][0]["role"],
              "pointcloud.ept");
    const json nothing = copyBuildArtifacts(fx.source.db.get(), fx.destination.db.get(), H_DSM);
    EXPECT_TRUE(nothing["artifacts"].empty());
    EXPECT_EQ(nothing["pending"], false);
}

TEST(BuildManifest, CopyBuildArtifactsRefusals) {
    EnvVarGuard noDiskFloor("DDB_MIN_FREE_DISK_MB", "0");
    CopyFixture fx(TEST_NAME);
    Database* src = fx.source.db.get();
    Database* dst = fx.destination.db.get();

    EXPECT_THROW(copyBuildArtifacts(src, dst, "nothex"), InvalidArgsException);
    EXPECT_THROW(copyBuildArtifacts(src, src, H_COG), InvalidArgsException);
    EXPECT_THROW(copyBuildArtifacts(src, dst, std::string(64, '0')), EntryNotFoundException);
    EXPECT_THROW(copyBuildArtifacts(src, dst, std::string(64, '1')), EntryNotFoundException)
        << "photo.jpg is not buildable";

    {
        // A build of the hash runs in the source: its output may be half published
        BuildLock running((fx.source.build / H_COG / "cog").string());
        EXPECT_THROW(copyBuildArtifacts(src, dst, H_COG), BuildInProgressException);
    }
    {
        // A build runs in the destination: nothing is replaced under it
        fs::create_directories(fx.destinationBuild / H_COG);
        BuildLock running((fx.destinationBuild / H_COG / "cog").string());
        EXPECT_THROW(copyBuildArtifacts(src, dst, H_COG), BuildInProgressException);
        EXPECT_FALSE(fs::exists(fx.destinationBuild / H_COG / "cog"));
    }
    {
        // A full disk is retryable
        EnvVarGuard floor("DDB_MIN_FREE_DISK_MB", "999999999999");
        try {
            copyBuildArtifacts(src, dst, H_COG);
            FAIL() << "expected RetryableBuildException";
        } catch (const RetryableBuildException& e) {
            EXPECT_EQ(e.reason(), RetryableBuildReason::LowDiskSpace);
        }
    }
    EXPECT_NO_THROW(copyBuildArtifacts(src, dst, H_COG));
    EXPECT_TRUE(fs::exists(fx.destinationBuild / H_COG / "cog" / "cog.tif"));
}

}  // namespace
