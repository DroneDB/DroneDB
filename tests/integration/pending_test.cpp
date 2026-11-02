/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Pending-build recovery: a retry is a full build attempt, the
// dependency names stored in ".pending" are dataset-root-relative, a build that
// awaits files is retried only once one of them is added (never on a timer), and
// dependencies that can never be satisfied fail the build.

#include <cstdint>
#include <chrono>
#include <cstdlib>
#include <iterator>
#include <fstream>
#include <memory>
#include <functional>
#include <string>
#include <vector>

#include "3d.h"
#include "timeutil.h"
#include "build.h"
#include "buildlock.h"
#include "buildlod_runner.h"
#include "dbops.h"
#include "ddb.h"
#include "exceptions.h"
#include "gdal_priv.h"
#include "gdal_write_failure.h"
#include "gtest/gtest.h"
#include "helpers/process_test_utils.h"
#include "in_process_raster_helper.h"
#include "ogr_api.h"
#include "ogr_srs_api.h"
#include "raster_helper.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"
#include "user_open_child.h"

namespace {

using namespace ddb;

std::string markerContents(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// A real 4x4 RGB PNG: bm3d refuses an invalid image, so a placeholder will not do.
const std::vector<uint8_t> kPng = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
    0x44, 0x52, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x08, 0x02, 0x00, 0x00,
    0x00, 0x26, 0x93, 0x09, 0x29, 0x00, 0x00, 0x00, 0x29, 0x49, 0x44, 0x41, 0x54, 0x78,
    0xda, 0x0d, 0xc7, 0x31, 0x01, 0x00, 0x00, 0x0c, 0xc2, 0x30, 0x84, 0x55, 0x18, 0x67,
    0x45, 0x21, 0x70, 0xcb, 0x97, 0x24, 0x12, 0x1b, 0x17, 0x13, 0x04, 0x8b, 0xe3, 0x53,
    0xa9, 0xad, 0xeb, 0x67, 0x32, 0x3b, 0x37, 0x0f, 0xa7, 0x63, 0x13, 0x41, 0x06, 0x1a,
    0x2b, 0x55, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

void writePng(const fs::path& p) {
    writeFile(p, kPng);
}

// A textured two-triangle OBJ referencing the given mtllib line (verbatim).
std::string objWithMtllib(const std::string& mtllibLine) {
    return mtllibLine + "\n"
           "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
           "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
           "usemtl mat\n"
           "f 1/1 2/2 3/3\nf 1/1 3/3 4/4\n";
}

std::string mtlWithTexture(const std::string& mapKdLine) {
    return "newmtl mat\nKa 1 1 1\nKd 1 1 1\n" + mapKdLine + "\n";
}

// An initialised dataset in a fresh TestArea.
struct Dataset : IndexFixture {
    explicit Dataset(const std::string& name) : IndexFixture(name, "") {}

    fs::path path(const std::string& rel) const { return root / fs::path(rel); }

    void add(const std::vector<std::string>& rels) {
        std::vector<std::string> abs;
        for (const auto& r : rels)
            abs.push_back(path(r).string());
        ddb::addToIndex(db.get(), abs);
    }

    fs::path pendingFile(const std::string& rel) {
        return db->buildDirectory() / (indexedHash(db.get(), rel) + ".pending");
    }

    std::vector<std::string> pendingDeps(const std::string& rel) {
        const auto hash = indexedHash(db.get(), rel);
        for (const auto& p : ddb::getPendingBuildInfo(db.get()))
            if (p.hash == hash)
                return p.missingDependencies;
        return {};
    }

    // The marker of rel awaits files (getPendingBuildInfo awaitingFiles); false without marker
    bool awaitingFiles(const std::string& rel) {
        const auto hash = indexedHash(db.get(), rel);
        for (const auto& p : ddb::getPendingBuildInfo(db.get()))
            if (p.hash == hash)
                return p.awaitingFiles;
        return false;
    }

    // The marker of rel is in its released form: due at once, awaiting no file
    bool released(const std::string& rel) {
        return fs::exists(pendingFile(rel)) && markerContents(pendingFile(rel)) == "0\n";
    }

    // Rewrites the timestamp of a ".pending" marker so the entry is past the backoff.
    void backdatePending(const std::string& rel, time_t secondsAgo = 900) {
        const fs::path p = pendingFile(rel);
        ASSERT_TRUE(fs::exists(p)) << p.string();
        std::vector<std::string> lines;
        {
            std::ifstream in(p.string());
            std::string line;
            while (std::getline(in, line))
                lines.push_back(line);
        }
        ASSERT_FALSE(lines.empty());
        lines[0] = std::to_string(ddb::currentUnixTimestamp() - secondsAgo);
        std::ofstream out(p.string(), std::ios::trunc);
        for (const auto& l : lines)
            out << l << "\n";
    }

    void writePendingMarker(const std::string& rel, time_t ts, const std::vector<std::string>& deps) {
        fs::create_directories(db->buildDirectory());
        std::ofstream out(pendingFile(rel).string(), std::ios::trunc);
        out << ts << "\n";
        for (const auto& d : deps)
            out << d << "\n";
    }

    bool bm3dBuilt(const std::string& rel) {
        return fs::exists(db->buildDirectory() / indexedHash(db.get(), rel) / "bm3d" /
                          "model.bm3d");
    }
};

// Builds a single entry and asserts it is deferred (not failed).
void expectDeferred(Dataset& ds, const std::string& rel) {
    EXPECT_THROW(ddb::build(ds.db.get(), rel, "", false), BuildDepMissingException);
    EXPECT_TRUE(fs::exists(ds.pendingFile(rel))) << "a deferred build writes .pending";
    EXPECT_FALSE(ddb::isBuildComplete(ds.db.get(), rel));
}

// Builds a single entry and asserts it fails outright: plain AppException, no ".pending".
void expectFailedWithoutPending(Dataset& ds, const std::string& rel, const std::string& mentions) {
    try {
        ddb::build(ds.db.get(), rel, "", false);
        FAIL() << "build of " << rel << " should fail";
    } catch (const BuildDepMissingException& e) {
        FAIL() << "unsatisfiable reference must not defer the build: " << e.what();
    } catch (const AppException& e) {
        EXPECT_NE(std::string(e.what()).find(mentions), std::string::npos) << e.what();
    }
    EXPECT_FALSE(fs::exists(ds.pendingFile(rel))) << "no .pending for an unsatisfiable reference";
    EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
}

// OBJ + MTL + PNG whose companions arrive after the model: each add releases the
// marker, the next buildPending retries it at once (no backoff to wait for), and
// the bm3d output is built once both exist.
void runCompanionsArriveLater(const std::string& folder) {
    Dataset ds(TEST_NAME);
    const std::string prefix = folder.empty() ? "" : folder + "/";
    const std::string obj = prefix + "m.obj";

    writeFile(ds.path(obj), objWithMtllib("mtllib m.mtl"));
    ds.add({obj});

    expectDeferred(ds, obj);
    EXPECT_EQ(ds.pendingDeps(obj), std::vector<std::string>{prefix + "m.mtl"});
    EXPECT_TRUE(ds.awaitingFiles(obj));
    EXPECT_FALSE(ddb::isBuildPending(ds.db.get())) << "nothing a retry can progress";

    // MTL arrives, texture still missing: released, retried, pending again on the texture.
    writeFile(ds.path(prefix + "m.mtl"), mtlWithTexture("map_Kd tex.png"));
    ds.add({prefix + "m.mtl"});
    EXPECT_TRUE(ds.released(obj));
    EXPECT_TRUE(ddb::isBuildPending(ds.db.get()));
    ddb::buildPending(ds.db.get(), "", false);
    EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
    EXPECT_EQ(ds.pendingDeps(obj), std::vector<std::string>{prefix + "tex.png"});
    EXPECT_TRUE(ds.awaitingFiles(obj));
    EXPECT_FALSE(ddb::isBuildComplete(ds.db.get(), obj));

    // Texture arrives: the retry builds the model and clears the marker.
    writePng(ds.path(prefix + "tex.png"));
    ds.add({prefix + "tex.png"});
    EXPECT_TRUE(ds.released(obj));
    ddb::buildPending(ds.db.get(), "", false);

    EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
    EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), obj));
    EXPECT_TRUE(ds.bm3dBuilt(obj));
}

TEST(PendingBuild, ObjAtRootClearsOnceCompanionsExist) {
    runCompanionsArriveLater("");
}

TEST(PendingBuild, ObjInSubfolderClearsOnceCompanionsExist) {
    runCompanionsArriveLater("sub");
}

TEST(PendingBuild, ObjInNestedSubfolderClearsOnceCompanionsExist) {
    runCompanionsArriveLater("a/b");
}

// Markers written by older versions store names relative to the OBJ's folder: the
// add of the file releases them all the same (by file name), and the retry
// rebuilds and re-validates on disk.
TEST(PendingBuild, OldFormatPendingClearsOnceDependencyExists) {
    Dataset ds(TEST_NAME);
    writeFile(ds.path("sub/m.obj"), objWithMtllib("mtllib m.mtl"));
    writeFile(ds.path("sub/m.mtl"), mtlWithTexture("map_Kd tex/a.png"));
    ds.add({"sub/m.obj", "sub/m.mtl"});

    ds.writePendingMarker("sub/m.obj", ddb::currentUnixTimestamp() - 900, {"tex/a.png"});
    EXPECT_TRUE(ds.awaitingFiles("sub/m.obj"));

    // Awaits a file: no retry without force
    ddb::buildPending(ds.db.get(), "", false);
    EXPECT_EQ(ds.pendingDeps("sub/m.obj"), std::vector<std::string>{"tex/a.png"});

    // Still missing: a forced retry rewrites the marker with the dataset-relative name.
    ddb::buildPending(ds.db.get(), "", true);
    EXPECT_EQ(ds.pendingDeps("sub/m.obj"), std::vector<std::string>{"sub/tex/a.png"});

    ds.writePendingMarker("sub/m.obj", ddb::currentUnixTimestamp() - 900, {"tex/a.png"});
    writePng(ds.path("sub/tex/a.png"));
    ds.add({"sub/tex/a.png"});
    EXPECT_TRUE(ds.released("sub/m.obj"));
    ddb::buildPending(ds.db.get(), "", false);

    EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
    EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "sub/m.obj"));
    EXPECT_TRUE(ds.bm3dBuilt("sub/m.obj"));
}

// The retry backoff of a build that awaits no file is 60 s since the last attempt;
// force bypasses it.
TEST(PendingBuild, BackoffIsSixtySecondsAndForceBypassesIt) {
    Dataset ds(TEST_NAME);
    writeFile(ds.path("m.obj"), objWithMtllib("mtllib m.mtl"));
    writeFile(ds.path("m.mtl"), mtlWithTexture("map_Kd tex.png"));
    writePng(ds.path("tex.png"));
    ds.add({"m.obj", "m.mtl", "tex.png"});

    // Attempted 10 s ago: not due.
    ds.writePendingMarker("m.obj", ddb::currentUnixTimestamp() - 10, {});
    ddb::buildPending(ds.db.get(), "", false);
    EXPECT_TRUE(fs::exists(ds.pendingFile("m.obj")));
    EXPECT_FALSE(ddb::isBuildComplete(ds.db.get(), "m.obj"));

    // Attempted 120 s ago: due (the old backoff was 300 s).
    ds.backdatePending("m.obj", 120);
    ddb::buildPending(ds.db.get(), "", false);
    EXPECT_FALSE(fs::exists(ds.pendingFile("m.obj")));
    EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "m.obj"));

    // A fresh marker is retried at once with force.
    ds.writePendingMarker("m.obj", ddb::currentUnixTimestamp(), {});
    ddb::buildPending(ds.db.get(), "", true);
    EXPECT_FALSE(fs::exists(ds.pendingFile("m.obj")));
    EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "m.obj"));
}

// An attempt that finds the output already complete still consumes ".pending",
// whatever the marker lists.
TEST(PendingBuild, CompleteOutputConsumesPendingMarker) {
    Dataset ds(TEST_NAME);
    writeFile(ds.path("m.obj"), objWithMtllib("mtllib m.mtl"));
    writeFile(ds.path("m.mtl"), mtlWithTexture("map_Kd tex.png"));
    writePng(ds.path("tex.png"));
    ds.add({"m.obj", "m.mtl", "tex.png"});

    ddb::build(ds.db.get(), "m.obj", "", false);
    ASSERT_TRUE(ddb::isBuildComplete(ds.db.get(), "m.obj"));

    // A due marker that awaits no file
    ds.writePendingMarker("m.obj", ddb::currentUnixTimestamp() - 900, {});
    ddb::buildPending(ds.db.get(), "", false);
    EXPECT_FALSE(fs::exists(ds.pendingFile("m.obj")));

    // A marker that awaits a file is not retried, but an attempt consumes it
    ds.writePendingMarker("m.obj", ddb::currentUnixTimestamp() - 900, {"not-indexed.png"});
    ddb::buildPending(ds.db.get(), "", false);
    EXPECT_TRUE(fs::exists(ds.pendingFile("m.obj")));
    ddb::build(ds.db.get(), "m.obj", "", false);

    EXPECT_FALSE(fs::exists(ds.pendingFile("m.obj")));
    EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
    EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "m.obj"));
}

// A retry that fails for a non-dependency reason consumes the marker (the
// failure is logged; DDBBuild(pendingOnly) still succeeds).
TEST(PendingBuild, NonDependencyFailureConsumesMarker) {
    Dataset ds(TEST_NAME);
    writeFile(ds.path("m.obj"), objWithMtllib("mtllib m.mtl"));
    ds.add({"m.obj"});
    expectDeferred(ds, "m.obj");

    // The MTL now references an absolute texture: unsatisfiable.
    writeFile(ds.path("m.mtl"), mtlWithTexture("map_Kd /nonexistent/tex.png"));
    ds.add({"m.mtl"});
    ds.backdatePending("m.obj");

    EXPECT_EQ(DDBBuild(ds.root.string().c_str(), nullptr, nullptr, false, true), DDBERR_NONE);
    EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
    EXPECT_FALSE(ddb::isBuildComplete(ds.db.get(), "m.obj"));
}

// Only top-level "<hash>.pending" files are markers; anything else is ignored.
TEST(PendingBuild, IgnoresNonHashAndNestedPendingFiles) {
    Dataset ds(TEST_NAME);
    writeFile(ds.path("m.obj"), objWithMtllib("mtllib m.mtl"));
    ds.add({"m.obj"});

    const fs::path buildDir = ds.db->buildDirectory();
    fs::create_directories(buildDir / "somefolder");
    writeFile(buildDir / "notes.pending", "0\n");
    writeFile(buildDir / "somefolder" / (indexedHash(ds.db.get(), "m.obj") + ".pending"), "0\nm.mtl\n");

    ddb::buildPending(ds.db.get(), "", false);

    EXPECT_TRUE(fs::exists(buildDir / "notes.pending"));
    EXPECT_TRUE(fs::exists(buildDir / "somefolder" / (indexedHash(ds.db.get(), "m.obj") + ".pending")));
    EXPECT_FALSE(fs::exists(ds.pendingFile("m.obj"))) << "nested marker must not be retried";
}

// ---------------------------------------------------------------------------
// Path contract
// ---------------------------------------------------------------------------

// mtllib mats/m.mtl + map_Kd tex.png: satisfied by mats/tex.png (MTL folder) ...
TEST(PendingBuild, MtlTextureFoundInMtlFolder) {
    Dataset ds(TEST_NAME);
    writeFile(ds.path("sub/m.obj"), objWithMtllib("mtllib mats/m.mtl"));
    writeFile(ds.path("sub/mats/m.mtl"), mtlWithTexture("map_Kd tex.png"));
    writePng(ds.path("sub/mats/tex.png"));
    ds.add({"sub/m.obj", "sub/mats/m.mtl", "sub/mats/tex.png"});

    EXPECT_EQ(getObjDependencies(ds.path("sub/m.obj").string(), ds.root.string()),
              (std::vector<std::string>{"mats/m.mtl", "mats/tex.png"}));
    ASSERT_NO_THROW(ddb::build(ds.db.get(), "sub/m.obj", "", false));
    EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "sub/m.obj"));
    EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
}

// ... and by <OBJ folder>/tex.png ...
TEST(PendingBuild, MtlTextureFoundInObjFolder) {
    Dataset ds(TEST_NAME);
    writeFile(ds.path("sub/m.obj"), objWithMtllib("mtllib mats/m.mtl"));
    writeFile(ds.path("sub/mats/m.mtl"), mtlWithTexture("map_Kd tex.png"));
    writePng(ds.path("sub/tex.png"));
    ds.add({"sub/m.obj", "sub/mats/m.mtl", "sub/tex.png"});

    ASSERT_NO_THROW(ddb::build(ds.db.get(), "sub/m.obj", "", false));
    EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "sub/m.obj"));
    EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
}

// ... and with neither, ".pending" lists the texture dataset-root-relative,
// relative to the MTL's folder.
TEST(PendingBuild, MissingMtlTextureListedDatasetRelative) {
    Dataset ds(TEST_NAME);
    writeFile(ds.path("m.obj"), objWithMtllib("mtllib mats/m.mtl"));
    writeFile(ds.path("mats/m.mtl"), mtlWithTexture("map_Kd tex.png"));
    writeFile(ds.path("sub/m.obj"), objWithMtllib("mtllib mats/m.mtl"));
    writeFile(ds.path("sub/mats/m.mtl"), mtlWithTexture("map_Kd tex.png"));
    ds.add({"m.obj", "mats/m.mtl", "sub/m.obj", "sub/mats/m.mtl"});

    expectDeferred(ds, "m.obj");
    EXPECT_EQ(ds.pendingDeps("m.obj"), std::vector<std::string>{"mats/tex.png"});

    expectDeferred(ds, "sub/m.obj");
    EXPECT_EQ(ds.pendingDeps("sub/m.obj"), std::vector<std::string>{"sub/mats/tex.png"});
}

TEST(PendingBuild, BackslashReferencesAreNormalised) {
    Dataset ds(TEST_NAME);
    writeFile(ds.path("sub/m.obj"), objWithMtllib("mtllib mats\\m.mtl"));
    writeFile(ds.path("sub/mats/m.mtl"), mtlWithTexture("map_Kd tex\\a.png"));
    ds.add({"sub/m.obj", "sub/mats/m.mtl"});

    EXPECT_EQ(getObjDependencies(ds.path("sub/m.obj").string(), ds.root.string()),
              (std::vector<std::string>{"mats/m.mtl", "mats/tex/a.png"}));

    expectDeferred(ds, "sub/m.obj");
    EXPECT_EQ(ds.pendingDeps("sub/m.obj"), std::vector<std::string>{"sub/mats/tex/a.png"});

    writePng(ds.path("sub/mats/tex/a.png"));
    ds.add({"sub/mats/tex/a.png"});
    ASSERT_NO_THROW(ddb::build(ds.db.get(), "sub/m.obj", "", false));
    EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "sub/m.obj"));
}

// "../" that stays inside the dataset is a normal dependency.
TEST(PendingBuild, ParentReferenceInsideDatasetAllowed) {
    Dataset ds(TEST_NAME);
    writeFile(ds.path("sub/m.obj"), objWithMtllib("mtllib ../shared/m.mtl"));
    ds.add({"sub/m.obj"});

    EXPECT_EQ(getObjDependencies(ds.path("sub/m.obj").string(), ds.root.string()),
              std::vector<std::string>{"../shared/m.mtl"});

    expectDeferred(ds, "sub/m.obj");
    EXPECT_EQ(ds.pendingDeps("sub/m.obj"), std::vector<std::string>{"shared/m.mtl"});

    writeFile(ds.path("shared/m.mtl"), mtlWithTexture("map_Kd ../shared/tex.png"));
    writePng(ds.path("shared/tex.png"));
    ds.add({"shared/m.mtl", "shared/tex.png"});
    ds.backdatePending("sub/m.obj");
    ddb::buildPending(ds.db.get(), "", false);

    EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
    EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "sub/m.obj"));
}

TEST(PendingBuild, AbsoluteMtllibFailsWithoutPending) {
    Dataset ds(TEST_NAME);
    writeFile(ds.path("m.obj"), objWithMtllib("mtllib /nonexistent/m.mtl"));
    ds.add({"m.obj"});
    expectFailedWithoutPending(ds, "m.obj", "/nonexistent/m.mtl");
}

TEST(PendingBuild, WindowsAbsoluteTextureFailsWithoutPending) {
    Dataset ds(TEST_NAME);
    writeFile(ds.path("m.obj"), objWithMtllib("mtllib m.mtl"));
    writeFile(ds.path("m.mtl"), mtlWithTexture("map_Kd C:\\textures\\tex.png"));
    ds.add({"m.obj", "m.mtl"});
    expectFailedWithoutPending(ds, "m.obj", "C:\\textures\\tex.png");
}

TEST(PendingBuild, RootEscapingReferenceFailsWithoutPending) {
    Dataset ds(TEST_NAME);
    writeFile(ds.path("sub/m.obj"), objWithMtllib("mtllib ../../m.mtl"));
    ds.add({"sub/m.obj"});

    // Without a dataset root the reference is just a relative path.
    EXPECT_EQ(getObjDependencies(ds.path("sub/m.obj").string()),
              std::vector<std::string>{"../../m.mtl"});
    EXPECT_THROW(getObjDependencies(ds.path("sub/m.obj").string(), ds.root.string()), AppException);

    expectFailedWithoutPending(ds, "sub/m.obj", "../../m.mtl");
}

TEST(PendingBuild, RootEscapingTextureFailsWithoutPending) {
    Dataset ds(TEST_NAME);
    writeFile(ds.path("m.obj"), objWithMtllib("mtllib m.mtl"));
    writeFile(ds.path("m.mtl"), mtlWithTexture("map_Kd ../outside/tex.png"));
    ds.add({"m.obj", "m.mtl"});
    expectFailedWithoutPending(ds, "m.obj", "../outside/tex.png");
}

TEST(PendingBuild, QuotedNamesWithSpaces) {
    Dataset ds(TEST_NAME);
    writeFile(ds.path("sub/my model.obj"), objWithMtllib("mtllib \"my mats/my model.mtl\""));
    writeFile(ds.path("sub/my mats/my model.mtl"), mtlWithTexture("map_Kd \"my tex.png\""));
    ds.add({"sub/my model.obj", "sub/my mats/my model.mtl"});

    EXPECT_EQ(getObjDependencies(ds.path("sub/my model.obj").string(), ds.root.string()),
              (std::vector<std::string>{"my mats/my model.mtl", "my mats/my tex.png"}));

    expectDeferred(ds, "sub/my model.obj");
    EXPECT_EQ(ds.pendingDeps("sub/my model.obj"),
              std::vector<std::string>{"sub/my mats/my tex.png"});

    writePng(ds.path("sub/my tex.png"));
    ds.add({"sub/my tex.png"});
    ASSERT_NO_THROW(ddb::build(ds.db.get(), "sub/my model.obj", "", false));
    EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "sub/my model.obj"));
}

// buildBm3d (validateDependencies) reports missing names relative to the OBJ's folder.
TEST(PendingBuild, ValidateDependenciesReportsMissingFile) {
    TestArea ta(TEST_NAME, true);
    const fs::path obj = ta.getPath("m.obj");
    writeFile(obj, objWithMtllib("mtllib m.mtl"));
    writeFile(ta.getPath("m.mtl"), mtlWithTexture("map_Kd tex.png\nmap_bump tex.png"));

    try {
        buildBm3d(obj.string(), ta.getPath("m.bm3d").string(), true);
        FAIL() << "buildBm3d should report the missing texture";
    } catch (const BuildDepMissingException& e) {
        EXPECT_EQ(e.getMissingDependencies(), std::vector<std::string>{"tex.png"});
    }
    EXPECT_FALSE(fs::exists(ta.getPath("m.bm3d")));
}

// ---------------------------------------------------------------------------
// Shapefile sidecars
// ---------------------------------------------------------------------------

// Writes a one-point EPSG:4326 shapefile (.shp/.shx/.dbf/.prj).
void writeShapefile(const fs::path& shp) {
    fs::create_directories(shp.parent_path());
    GDALAllRegister();
    GDALDriverH drv = GDALGetDriverByName("ESRI Shapefile");
    ASSERT_NE(drv, nullptr);
    GDALDatasetH ds = GDALCreate(drv, shp.string().c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    ASSERT_NE(ds, nullptr);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 4326);
    OGRLayerH layer = GDALDatasetCreateLayer(ds, shp.stem().string().c_str(), srs, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);
    OGRFieldDefnH fld = OGR_Fld_Create("name", OFTString);
    OGR_L_CreateField(layer, fld, TRUE);
    OGR_Fld_Destroy(fld);
    OGRFeatureH feat = OGR_F_Create(OGR_L_GetLayerDefn(layer));
    OGR_F_SetFieldString(feat, 0, "p");
    OGRGeometryH pt = OGR_G_CreateGeometry(wkbPoint);
    OGR_G_SetPoint_2D(pt, 0, 12.5, 41.9);
    OGR_F_SetGeometry(feat, pt);
    OGR_G_DestroyGeometry(pt);
    ASSERT_EQ(OGR_L_CreateFeature(layer, feat), OGRERR_NONE);
    OGR_F_Destroy(feat);
    OSRDestroySpatialReference(srs);
    GDALClose(ds);
}

// Sets up sub/layer.shp indexed and deferred because sub/layer.shx is missing,
// then adds sub/layer.shx, which releases the marker.
void deferShapefileInSubfolder(Dataset& ds) {
    writeShapefile(ds.path("sub/layer.shp"));
    const fs::path shx = ds.path("sub/layer.shx");
    const fs::path stash = ds.ta.getPath("layer.shx.stash");
    ASSERT_TRUE(fs::exists(shx));
    fs::rename(shx, stash);

    ds.add({"sub/layer.shp", "sub/layer.dbf", "sub/layer.prj"});
    expectDeferred(ds, "sub/layer.shp");
    EXPECT_EQ(ds.pendingDeps("sub/layer.shp"), std::vector<std::string>{"sub/layer.shx"});

    fs::rename(stash, shx);
    ds.add({"sub/layer.shx"});
    EXPECT_TRUE(ds.released("sub/layer.shp"));
}

TEST(PendingBuild, ShapefileInSubfolderClearsViaBuildPending) {
    Dataset ds(TEST_NAME);
    deferShapefileInSubfolder(ds);

    ddb::buildPending(ds.db.get(), "", false);

    EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
    EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "sub/layer.shp"));
}

// The Registry builds the sidecar entry when it is indexed: that must build the
// main file in the same folder.
TEST(PendingBuild, ShapefileSidecarInSubfolderBuildsMainFile) {
    Dataset ds(TEST_NAME);
    deferShapefileInSubfolder(ds);

    EXPECT_EQ(ddb::buildSubfolder(ds.db.get(), "sub/layer.shx"), "vec");

    ASSERT_NO_THROW(ddb::build(ds.db.get(), "sub/layer.shx", "", false));

    EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
    EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "sub/layer.shp"));
    EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "sub/layer.shx"));
    const fs::path out = ds.db->buildDirectory() / indexedHash(ds.db.get(), "sub/layer.shp");
    EXPECT_TRUE(fs::exists(out / "vec" / "source.gpkg"));
    EXPECT_TRUE(fs::exists(out / "mvt" / "metadata.json"));
}

USER_OPEN_TEST(PendingBuild, VectorNoSpaceRetainsReleasedPendingMarkerWithFreshTimestamp) {
    for (const std::string driver : {"MVT", "GPKG"})
        for (const bool onClose : {false, true}) {
            SCOPED_TRACE(driver + (onClose ? " close" : " translate"));
            // Short area name: hash + .vec-stage-* + MVT tile dirs must stay under
            // the Windows CreateDirectory limit (MAX_PATH - 12).
            Dataset ds("PendingBuild-VecNoSpace-" + driver + (onClose ? "-close" : "-translate"));
            deferShapefileInSubfolder(ds); // dependency was missing, then arrived
            // The test makes the write of a GDAL driver of this process fail
            testutils::InProcessRasterHelper helper;
            const raster::ScopedRasterHelper installed(helper);
            const auto before = ddb::currentUnixTimestamp();
            {
                EnvVarGuard enoughDisk("DDB_MIN_FREE_DISK_MB", "0");
                ddb_test::withVectorWriteFailure(driver.c_str(), onClose, [&] {
                    EXPECT_THROW(ddb::build(ds.db.get(), "sub/layer.shp", "", false), RetryableBuildException);
                });
            }
            const auto pending = getPendingBuildInfo(ds.db.get());
            ASSERT_EQ(pending.size(), 1u);
            EXPECT_GE(pending[0].lastAttempt, before);
            // The add of the .shx released it: it awaits no file, a timer retries it
            EXPECT_TRUE(pending[0].missingDependencies.empty());
            EXPECT_FALSE(pending[0].awaitingFiles);
            EXPECT_TRUE(isBuildPending(ds.db.get()));
            EXPECT_FALSE(isBuildComplete(ds.db.get(), "sub/layer.shp"));
            // The restored real driver can complete the same pending entry.
            ds.backdatePending("sub/layer.shp", 60);
            EnvVarGuard enoughDisk("DDB_MIN_FREE_DISK_MB", "0");
            EXPECT_NO_THROW(buildPending(ds.db.get(), "", false));
            EXPECT_FALSE(fs::exists(ds.pendingFile("sub/layer.shp")));
            EXPECT_TRUE(isBuildComplete(ds.db.get(), "sub/layer.shp"));
        }
}

TEST(PendingBuild, PrimaryIdleTimeoutRetainsMarkerThroughFailedEntryBuild) {
    Dataset ds(TEST_NAME);
    const fs::path input = ds.path("sample.splat");
    {
        // One valid Gaussian splat: XYZ, scales, RGBA and an identity quaternion.
        std::ofstream out(input, std::ios::binary);
        const float values[6] = {0, 0, 0, 0.1f, 0.1f, 0.1f};
        const uint8_t attributes[8] = {255, 180, 90, 255, 255, 128, 128, 128};
        out.write(reinterpret_cast<const char*>(values), sizeof(values));
        out.write(reinterpret_cast<const char*>(attributes), sizeof(attributes));
    }
    ds.add({"sample.splat"});
    Entry entry;
    ASSERT_TRUE(loadEntry(ds.db.get(), "sample.splat", entry));
    ASSERT_EQ(entry.type, EntryType::GaussianSplat);
    ds.writePendingMarker("sample.splat", 1, {"previous dependency hint"});
    const auto realTool = buildlod::findBuildLodBinary(true);
    const auto before = ddb::currentUnixTimestamp();
    const auto start = std::chrono::steady_clock::now();
    {
        EnvVarGuard tool("DDB_BUILDLOD_PATH", testChildPath().string().c_str());
        EnvVarGuard mode("DDB_TEST_CHILD_MODE", "idle");
        EnvVarGuard deadline("DDB_BUILDLOD_IDLE_TIMEOUT_SECONDS", "1");
        EnvVarGuard enoughDisk("DDB_MIN_FREE_DISK_MB", "0");
        buildlod::findBuildLodBinary(true);
        try {
            build(ds.db.get(), "sample.splat", "", false);
            FAIL() << "the primary tool should time out";
        } catch (const RetryableBuildException& exception) {
            EXPECT_EQ(exception.reason(), RetryableBuildReason::IdleTimeout);
        }
        EXPECT_EQ(DDBBuild(ds.root.string().c_str(), "sample.splat", nullptr, false, false), DDBERR_EXCEPTION);
    }
    buildlod::findBuildLodBinary(true);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(15));
    const auto pending = getPendingBuildInfo(ds.db.get());
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_GE(pending[0].lastAttempt, before);
    EXPECT_EQ(pending[0].missingDependencies, (std::vector<std::string>{"previous dependency hint"}));
    // It still awaits that file: the failed attempt does not turn it into timer work
    EXPECT_TRUE(pending[0].awaitingFiles);
    EXPECT_FALSE(isBuildPending(ds.db.get()));
    EXPECT_FALSE(isBuildComplete(ds.db.get(), "sample.splat"));
    const fs::path output = ds.db->buildDirectory() / entry.hash;
    EXPECT_FALSE(fs::exists(output / "gsplat" / "model.rad"));
    for (const auto& child : fs::directory_iterator(output))
        EXPECT_EQ(child.path().filename().string().find("gsplat-temp-"), std::string::npos);
    if (!realTool.empty()) {
        EnvVarGuard tool("DDB_BUILDLOD_PATH", realTool.string().c_str());
        EnvVarGuard enoughDisk("DDB_MIN_FREE_DISK_MB", "0");
        buildlod::findBuildLodBinary(true);
        EXPECT_NO_THROW(buildPending(ds.db.get(), "", true));
        EXPECT_FALSE(fs::exists(ds.pendingFile("sample.splat")));
        EXPECT_TRUE(isBuildComplete(ds.db.get(), "sample.splat"));
    }
    buildlod::findBuildLodBinary(true);
}

TEST(PendingBuild, InvalidBuildLodDeadlineIsPermanentAndDoesNotTouchRunnerOutputs) {
    Dataset ds(TEST_NAME);
    const fs::path input = ds.path("sample.splat");
    writeFile(input, std::string(32, '\0'));
    const fs::path output = ds.path("previous.rad");
    const fs::path adjacent = ds.path("sample-lod.rad");
    for (const char* invalid : {"0", "-1", "+1", "1.5", " 1", "1 ", "abc", "86401", "9223372037", "18446744073709551616"}) {
        SCOPED_TRACE(invalid);
        writeFile(output, "previous output");
        writeFile(adjacent, "adjacent output");
        EnvVarGuard deadline("DDB_BUILDLOD_IDLE_TIMEOUT_SECONDS", invalid);
        const auto result = buildlod::runBuildLodResult({input, output, true, 1, testChildPath()});
        EXPECT_FALSE(result.succeeded);
        EXPECT_FALSE(result.timedOut);
        EXPECT_NE(result.error.find("DDB_BUILDLOD_IDLE_TIMEOUT_SECONDS"), std::string::npos);
        EXPECT_NO_THROW(result.throwIfRetryable("primary: "));
        EXPECT_EQ(markerContents(output), "previous output");
        EXPECT_EQ(markerContents(adjacent), "adjacent output");
    }
    ds.add({"sample.splat"});
    ds.writePendingMarker("sample.splat", 1, {"previous hint"});
    {
        EnvVarGuard tool("DDB_BUILDLOD_PATH", testChildPath().string().c_str());
        EnvVarGuard deadline("DDB_BUILDLOD_IDLE_TIMEOUT_SECONDS", "invalid");
        EnvVarGuard enoughDisk("DDB_MIN_FREE_DISK_MB", "0");
        buildlod::findBuildLodBinary(true);
        EXPECT_EQ(DDBBuild(ds.root.string().c_str(), "sample.splat", nullptr, false, false), DDBERR_EXCEPTION);
        EXPECT_FALSE(fs::exists(ds.pendingFile("sample.splat")));
    }
    buildlod::findBuildLodBinary(true);
}

TEST(PendingBuild, MaximumBuildLodDeadlineParsesAndLaunchesTool) {
    Dataset ds(TEST_NAME);
    const fs::path input = ds.path("sample.splat");
    const fs::path output = ds.path("model.rad");
    writeFile(input, std::string(32, '\0'));
    EnvVarGuard deadline("DDB_BUILDLOD_IDLE_TIMEOUT_SECONDS", "86400");
    process::Options options;
    options.envOverrides.emplace_back("DDB_TEST_CHILD_MODE", "no-output");
    const auto started = std::chrono::steady_clock::now();
    const auto result = buildlod::runBuildLodResult({input, output, true, 1, testChildPath()}, options);
    // Exit 0 with no output proves parsing accepted the upper bound and launched
    // the child. The missing-output contract still makes this a permanent failure.
    EXPECT_FALSE(result.succeeded);
    EXPECT_FALSE(result.timedOut);
    EXPECT_NE(result.error.find("reported success but expected output is missing"), std::string::npos);
    EXPECT_NO_THROW(result.throwIfRetryable("primary: "));
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(10));
}

// A pending primary failure stays visibly failed while its marker (released by the
// add of the dependencies) and refreshed timestamp let a delayed coordinator retry
// the same entry.
TEST(PendingBuild, LowDiskRetainsPendingThenSucceedsAtSixtySecondBoundary) {
    Dataset ds(TEST_NAME);
    writeFile(ds.path("m.obj"), objWithMtllib("mtllib m.mtl"));
    ds.add({"m.obj"});
    expectDeferred(ds, "m.obj");
    writeFile(ds.path("m.mtl"), mtlWithTexture("map_Kd tex.png"));
    writePng(ds.path("tex.png"));
    ds.add({"m.mtl", "tex.png"});
    EXPECT_TRUE(ds.released("m.obj"));
    const auto beforeAttempt = ddb::currentUnixTimestamp();
    {
        EnvVarGuard lowDisk("DDB_MIN_FREE_DISK_MB", "100000000000");
        EXPECT_THROW(ddb::build(ds.db.get(), "m.obj", "", false), RetryableBuildException);
        // The C ABI still signals an ordinary failed entry job.
        EXPECT_EQ(DDBBuild(ds.root.string().c_str(), "m.obj", nullptr, false, false), DDBERR_EXCEPTION);
    }
    const auto pending = ddb::getPendingBuildInfo(ds.db.get());
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_GE(pending[0].lastAttempt, beforeAttempt);
    EXPECT_TRUE(pending[0].missingDependencies.empty());
    EXPECT_FALSE(pending[0].awaitingFiles);
    EXPECT_FALSE(ds.bm3dBuilt("m.obj"));

    EnvVarGuard enoughDisk("DDB_MIN_FREE_DISK_MB", "0");
    ddb::buildPending(ds.db.get(), "", false);
    EXPECT_TRUE(fs::exists(ds.pendingFile("m.obj"))) << "fresh attempt is inside 60-second backoff";
    ds.backdatePending("m.obj", 60);
    ddb::buildPending(ds.db.get(), "", false);
    EXPECT_FALSE(fs::exists(ds.pendingFile("m.obj")));
    EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "m.obj"));
}

TEST(PendingBuild, FreshLowDiskFailureDoesNotCreatePendingMarker) {
    Dataset ds(TEST_NAME);
    writeFile(ds.path("m.obj"), objWithMtllib("mtllib missing.mtl"));
    ds.add({"m.obj"});
    EnvVarGuard lowDisk("DDB_MIN_FREE_DISK_MB", "100000000000");
    EXPECT_THROW(ddb::build(ds.db.get(), "m.obj", "", false), RetryableBuildException);
    EXPECT_FALSE(fs::exists(ds.pendingFile("m.obj")));
}

TEST(PendingBuild, PrimaryLockContentionLeavesMarkerByteForByteUnchanged) {
    Dataset ds(TEST_NAME);
    writeFile(ds.path("m.obj"), objWithMtllib("mtllib missing.mtl"));
    ds.add({"m.obj"});
    ds.writePendingMarker("m.obj", 1, {"missing.mtl", "older hint.png"});
    const auto original = markerContents(ds.pendingFile("m.obj"));
    const auto base = ds.db->buildDirectory() / indexedHash(ds.db.get(), "m.obj");
    fs::create_directories(base);
    BuildLock lock((base / "bm3d").string());
    EXPECT_THROW(ddb::build(ds.db.get(), "m.obj", "", false), BuildInProgressException);
    EXPECT_EQ(markerContents(ds.pendingFile("m.obj")), original);
}

TEST(PendingBuild, TilesLockContentionLeavesMarkerByteForByteUnchanged) {
    Dataset ds(TEST_NAME);
    writeFile(ds.path("m.obj"), objWithMtllib("mtllib missing.mtl"));
    ds.add({"m.obj"});
    ds.writePendingMarker("m.obj", 1, {"missing.mtl"});
    const auto original = markerContents(ds.pendingFile("m.obj"));
    const auto base = ds.db->buildDirectory() / indexedHash(ds.db.get(), "m.obj");
    fs::create_directories(base);
    BuildLock lock((base / "3dtiles").string());
    EXPECT_THROW(ddb::build(ds.db.get(), "m.obj", "", false), BuildInProgressException);
    EXPECT_EQ(markerContents(ds.pendingFile("m.obj")), original);
}

#ifndef _WIN32
TEST(PendingBuild, FailedAtomicTimestampReplacementPreservesOriginalMarker) {
    Dataset ds(TEST_NAME);
    writeFile(ds.path("m.obj"), objWithMtllib("mtllib missing.mtl"));
    ds.add({"m.obj"});
    ds.writePendingMarker("m.obj", 1, {"missing.mtl"});
    const auto original = markerContents(ds.pendingFile("m.obj"));
    fs::create_directories(ds.db->buildDirectory() / indexedHash(ds.db.get(), "m.obj"));
    const auto permissions = fs::status(ds.db->buildDirectory()).permissions();
    struct RestorePermissions {
        fs::path path;
        fs::perms permissions;
        ~RestorePermissions() { std::error_code ec; fs::permissions(path, permissions, ec); }
    } restore{ds.db->buildDirectory(), permissions};
    fs::permissions(ds.db->buildDirectory(), fs::perms::owner_write | fs::perms::group_write |
                    fs::perms::others_write, fs::perm_options::remove);
    // Root can bypass directory permissions; this test needs the failure injection.
    std::ofstream probe(ds.db->buildDirectory() / "permission-probe");
    if (probe.is_open()) {
        probe.close();
        fs::remove(ds.db->buildDirectory() / "permission-probe");
        GTEST_SKIP() << "Current user bypasses write permissions";
    }
    EnvVarGuard lowDisk("DDB_MIN_FREE_DISK_MB", "100000000000");
    EXPECT_THROW(ddb::build(ds.db.get(), "m.obj", "", false), RetryableBuildException);
    EXPECT_EQ(markerContents(ds.pendingFile("m.obj")), original);
}
#endif

// ---------------------------------------------------------------------------
// Builds that await files
// ---------------------------------------------------------------------------

// Defers m.obj (at folder) because its MTL is missing; the MTL it names has no texture.
void deferObjAwaitingMtl(Dataset& ds, const std::string& obj, const std::string& mtllib = "m.mtl") {
    writeFile(ds.path(obj), objWithMtllib("mtllib " + mtllib));
    ds.add({obj});
    expectDeferred(ds, obj);
    ASSERT_TRUE(ds.awaitingFiles(obj));
}

// A build that only awaits missing files is neither retried by buildPending (even
// long past the backoff) nor counted by isBuildPending; force still retries it.
TEST(PendingBuild, AwaitingMarkerIsNotRetriedNorCounted) {
    Dataset ds(TEST_NAME);
    deferObjAwaitingMtl(ds, "m.obj");
    ds.backdatePending("m.obj", 3600);
    const auto before = markerContents(ds.pendingFile("m.obj"));

    EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
    ddb::buildPending(ds.db.get(), "", false);
    EXPECT_EQ(markerContents(ds.pendingFile("m.obj")), before) << "not attempted";

    const auto info = ddb::getPendingBuildInfo(ds.db.get());
    ASSERT_EQ(info.size(), 1u);
    EXPECT_TRUE(info[0].awaitingFiles);
    EXPECT_EQ(info[0].missingDependencies, std::vector<std::string>{"m.mtl"});

    ddb::buildPending(ds.db.get(), "", true);
    EXPECT_NE(markerContents(ds.pendingFile("m.obj")), before) << "force attempts it";
    EXPECT_TRUE(ds.awaitingFiles("m.obj"));
}

// A marker without dependencies (a transient failure, or a released one) is
// counted and retried once due; beside it an awaiting marker is left alone.
TEST(PendingBuild, TransientMarkerIsRetriedBesideAnAwaitingOne) {
    Dataset ds(TEST_NAME);
    deferObjAwaitingMtl(ds, "a/m.obj");
    const auto awaiting = markerContents(ds.pendingFile("a/m.obj"));
    // Another content than a/m.obj: one hash, one marker
    writeFile(ds.path("b/m.obj"), objWithMtllib("mtllib b.mtl"));
    writeFile(ds.path("b/b.mtl"), mtlWithTexture("map_Kd tex.png"));
    writePng(ds.path("b/tex.png"));
    ds.add({"b/m.obj", "b/b.mtl", "b/tex.png"});
    ds.writePendingMarker("b/m.obj", ddb::currentUnixTimestamp() - 900, {});

    EXPECT_TRUE(ddb::isBuildPending(ds.db.get()));
    EXPECT_FALSE(ds.awaitingFiles("b/m.obj"));
    ddb::buildPending(ds.db.get(), "", false);

    EXPECT_TRUE(ds.bm3dBuilt("b/m.obj"));
    EXPECT_FALSE(fs::exists(ds.pendingFile("b/m.obj")));
    EXPECT_EQ(markerContents(ds.pendingFile("a/m.obj")), awaiting);
    EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
}

// The release matches the file name, case-insensitively, wherever the file lands:
// a name the builder cannot use costs one retry, which defers the build again.
TEST(PendingBuild, AddReleasesByFileNameIgnoringCaseAndFolder) {
    Dataset ds(TEST_NAME);
    deferObjAwaitingMtl(ds, "sub/m.obj", "Model.MTL");
    EXPECT_EQ(ds.pendingDeps("sub/m.obj"), std::vector<std::string>{"sub/Model.MTL"});

    writeFile(ds.path("elsewhere/model.mtl"), mtlWithTexture("map_Kd tex.png"));
    ds.add({"elsewhere/model.mtl"});
    EXPECT_TRUE(ds.released("sub/m.obj"));
    EXPECT_TRUE(ddb::isBuildPending(ds.db.get()));

    ddb::buildPending(ds.db.get(), "", false);
    EXPECT_TRUE(ds.awaitingFiles("sub/m.obj")) << "still missing where the OBJ looks: deferred again";
    EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
}

// Adding a file of another name, or a folder named like the dependency, leaves the
// marker byte for byte as it was.
TEST(PendingBuild, AddOfAnotherFileOrAFolderDoesNotRelease) {
    Dataset ds(TEST_NAME);
    deferObjAwaitingMtl(ds, "m.obj");
    const auto before = markerContents(ds.pendingFile("m.obj"));

    writeFile(ds.path("other.mtl"), mtlWithTexture("map_Kd tex.png"));
    writeFile(ds.path("x/m.mtl/readme.txt"), "a folder named like the dependency");
    ds.add({"other.mtl", "x"});

    EXPECT_EQ(markerContents(ds.pendingFile("m.obj")), before);
    EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
}

// addToIndexEx (DDBAddWithOptions) releases as addToIndex does.
TEST(PendingBuild, AddWithOptionsReleases) {
    Dataset ds(TEST_NAME);
    deferObjAwaitingMtl(ds, "m.obj");
    writeFile(ds.path("m.mtl"), mtlWithTexture("map_Kd tex.png"));
    ddb::AddResult result;
    ddb::addToIndexEx(ds.db.get(), {ds.path("m.mtl").string()}, ddb::AddOptions{}, result);
    ASSERT_EQ(result.entries.size(), 1u);
    EXPECT_TRUE(ds.released("m.obj"));
}

// Moving or renaming a file to the missing name releases the build, which then
// builds; so does a folder moved to where the dependency is looked for.
TEST(PendingBuild, MoveIntoTheMissingNameReleases) {
    Dataset ds(TEST_NAME);
    writePng(ds.path("tex.png"));
    ds.add({"tex.png"});
    deferObjAwaitingMtl(ds, "m.obj");
    writeFile(ds.path("upload.mtl"), mtlWithTexture("map_Kd tex.png"));
    ds.add({"upload.mtl"});
    EXPECT_TRUE(ds.awaitingFiles("m.obj")) << "another name";

    // As a host moves an entry: the file on disk first, then the index
    fs::rename(ds.path("upload.mtl"), ds.path("m.mtl"));
    ddb::moveEntry(ds.db.get(), "upload.mtl", "m.mtl");
    EXPECT_TRUE(ds.released("m.obj"));

    ddb::buildPending(ds.db.get(), "", false);
    EXPECT_TRUE(ds.bm3dBuilt("m.obj"));
    EXPECT_FALSE(fs::exists(ds.pendingFile("m.obj")));

    // A folder: mats/n.mtl arrives as upload/n.mtl, then upload is moved to mats. The
    // add already releases it (same file name), and the retry defers it again
    deferObjAwaitingMtl(ds, "n.obj", "mats/n.mtl");
    writeFile(ds.path("upload/n.mtl"), mtlWithTexture("map_Kd ../tex.png"));
    ds.add({"upload"});
    EXPECT_TRUE(ds.released("n.obj"));
    ddb::buildPending(ds.db.get(), "", false);
    ASSERT_TRUE(ds.awaitingFiles("n.obj"));
    fs::rename(ds.path("upload"), ds.path("mats"));
    ddb::moveEntry(ds.db.get(), "upload", "mats");
    EXPECT_TRUE(ds.released("n.obj"));

    ddb::buildPending(ds.db.get(), "", false);
    EXPECT_TRUE(ds.bm3dBuilt("n.obj"));
}

// A marker that an attempt writes back as it found it (a transient failure) is
// checked as well: its dependency is on disk by now, so it is released.
TEST(PendingBuild, RetainedAwaitingMarkerIsReleasedWhenItsDependencyExists) {
    Dataset ds(TEST_NAME);
    deferObjAwaitingMtl(ds, "m.obj");
    // On disk, never added (the add would have released it)
    writeFile(ds.path("m.mtl"), mtlWithTexture("map_Kd tex.png"));
    EXPECT_TRUE(ds.awaitingFiles("m.obj"));

    EnvVarGuard lowDisk("DDB_MIN_FREE_DISK_MB", "100000000000");
    EXPECT_THROW(ddb::build(ds.db.get(), "m.obj", "", false), RetryableBuildException);
    EXPECT_TRUE(ds.released("m.obj"));
}

}  // namespace
