/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Pending-build recovery (spec A1): a retry is a full build attempt, the
// dependency names stored in ".pending" are dataset-root-relative and for
// display only, and dependencies that can never be satisfied fail the build.

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
#include "build.h"
#include "buildlock.h"
#include "buildlod_runner.h"
#include "dbops.h"
#include "ddb.h"
#include "exceptions.h"
#include "gdal_priv.h"
#include "gtest/gtest.h"
#include "helpers/process_test_utils.h"
#include "ogr_api.h"
#include "ogr_srs_api.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace ddb_test {
void withVectorWriteFailure(const char* driver, bool onClose, const std::function<void()>& operation);
}

namespace {

using namespace ddb;

class ScopedRetryEnv {
public:
    ScopedRetryEnv(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name)) { hadValue_ = true; previous_ = old; }
        set(value);
    }
    ~ScopedRetryEnv() { set(hadValue_ ? previous_.c_str() : nullptr); }
private:
    void set(const char* value) {
#ifdef _WIN32
        _putenv_s(name_.c_str(), value ? value : "");
#else
        if (value) setenv(name_.c_str(), value, 1); else unsetenv(name_.c_str());
#endif
    }
    std::string name_, previous_;
    bool hadValue_ = false;
};

std::string markerContents(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// A real 4x4 RGB PNG: Nexus aborts on an invalid image, so a placeholder will not do.
const std::vector<uint8_t> kPng = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
    0x44, 0x52, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x08, 0x02, 0x00, 0x00,
    0x00, 0x26, 0x93, 0x09, 0x29, 0x00, 0x00, 0x00, 0x29, 0x49, 0x44, 0x41, 0x54, 0x78,
    0xda, 0x0d, 0xc7, 0x31, 0x01, 0x00, 0x00, 0x0c, 0xc2, 0x30, 0x84, 0x55, 0x18, 0x67,
    0x45, 0x21, 0x70, 0xcb, 0x97, 0x24, 0x12, 0x1b, 0x17, 0x13, 0x04, 0x8b, 0xe3, 0x53,
    0xa9, 0xad, 0xeb, 0x67, 0x32, 0x3b, 0x37, 0x0f, 0xa7, 0x63, 0x13, 0x41, 0x06, 0x1a,
    0x2b, 0x55, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

void writeText(const fs::path& p, const std::string& content) {
    if (p.has_parent_path())
        fs::create_directories(p.parent_path());
    std::ofstream out(p.string(), std::ios::binary);
    out << content;
}

void writePng(const fs::path& p) {
    if (p.has_parent_path())
        fs::create_directories(p.parent_path());
    std::ofstream out(p.string(), std::ios::binary);
    out.write(reinterpret_cast<const char*>(kPng.data()), static_cast<std::streamsize>(kPng.size()));
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
struct Dataset {
    explicit Dataset(const std::string& name) : ta(name, true) {
        root = ta.getFolder("");
        ddb::initIndex(root.string());
        db = ddb::open(root.string(), true);
    }

    fs::path path(const std::string& rel) const { return root / fs::path(rel); }

    void add(const std::vector<std::string>& rels) {
        std::vector<std::string> abs;
        for (const auto& r : rels)
            abs.push_back(path(r).string());
        ddb::addToIndex(db.get(), abs);
    }

    std::string hashOf(const std::string& rel) {
        Entry e;
        EXPECT_TRUE(ddb::getEntry(db.get(), rel, e)) << rel << " is not indexed";
        return e.hash;
    }

    fs::path pendingFile(const std::string& rel) {
        return db->buildDirectory() / (hashOf(rel) + ".pending");
    }

    std::vector<std::string> pendingDeps(const std::string& rel) {
        const auto hash = hashOf(rel);
        for (const auto& p : ddb::getPendingBuildInfo(db.get()))
            if (p.hash == hash)
                return p.missingDependencies;
        return {};
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
        lines[0] = std::to_string(utils::currentUnixTimestamp() - secondsAgo);
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

    bool nxzBuilt(const std::string& rel) {
        return fs::exists(db->buildDirectory() / hashOf(rel) / "nxs" / "model.nxz");
    }

    TestArea ta;
    fs::path root;
    std::unique_ptr<Database> db;
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

// OBJ + MTL + PNG whose companions arrive after the model: the pending state
// clears through buildPending once they exist, and the Nexus output is built.
void runCompanionsArriveLater(const std::string& folder) {
    Dataset ds(TEST_NAME);
    const std::string prefix = folder.empty() ? "" : folder + "/";
    const std::string obj = prefix + "m.obj";

    writeText(ds.path(obj), objWithMtllib("mtllib m.mtl"));
    ds.add({obj});

    expectDeferred(ds, obj);
    EXPECT_EQ(ds.pendingDeps(obj), std::vector<std::string>{prefix + "m.mtl"});

    // MTL arrives, texture still missing: still pending, now on the texture.
    writeText(ds.path(prefix + "m.mtl"), mtlWithTexture("map_Kd tex.png"));
    ds.add({prefix + "m.mtl"});
    ds.backdatePending(obj);
    ddb::buildPending(ds.db.get(), "", false);
    EXPECT_TRUE(ddb::isBuildPending(ds.db.get()));
    EXPECT_EQ(ds.pendingDeps(obj), std::vector<std::string>{prefix + "tex.png"});
    EXPECT_FALSE(ddb::isBuildComplete(ds.db.get(), obj));

    // Texture arrives: the retry builds the model and clears the marker.
    writePng(ds.path(prefix + "tex.png"));
    ds.add({prefix + "tex.png"});
    ds.backdatePending(obj);
    ddb::buildPending(ds.db.get(), "", false);

    EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
    EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), obj));
    EXPECT_TRUE(ds.nxzBuilt(obj));
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

// Markers written by older versions store names relative to the OBJ's folder.
// They are informational: the retry rebuilds and re-validates on disk.
TEST(PendingBuild, OldFormatPendingClearsOnceDependencyExists) {
    Dataset ds(TEST_NAME);
    writeText(ds.path("sub/m.obj"), objWithMtllib("mtllib m.mtl"));
    writeText(ds.path("sub/m.mtl"), mtlWithTexture("map_Kd tex/a.png"));
    ds.add({"sub/m.obj", "sub/m.mtl"});

    ds.writePendingMarker("sub/m.obj", utils::currentUnixTimestamp() - 900, {"tex/a.png"});

    // Still missing: the retry rewrites the marker with the dataset-relative name.
    ddb::buildPending(ds.db.get(), "", false);
    EXPECT_EQ(ds.pendingDeps("sub/m.obj"), std::vector<std::string>{"sub/tex/a.png"});

    writePng(ds.path("sub/tex/a.png"));
    ds.add({"sub/tex/a.png"});
    ds.backdatePending("sub/m.obj");
    ddb::buildPending(ds.db.get(), "", false);

    EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
    EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "sub/m.obj"));
    EXPECT_TRUE(ds.nxzBuilt("sub/m.obj"));
}

// The retry backoff is 60 s since the last attempt; force bypasses it.
TEST(PendingBuild, BackoffIsSixtySecondsAndForceBypassesIt) {
    Dataset ds(TEST_NAME);
    writeText(ds.path("m.obj"), objWithMtllib("mtllib m.mtl"));
    writeText(ds.path("m.mtl"), mtlWithTexture("map_Kd tex.png"));
    writePng(ds.path("tex.png"));
    ds.add({"m.obj", "m.mtl", "tex.png"});

    // Attempted 10 s ago: not due.
    ds.writePendingMarker("m.obj", utils::currentUnixTimestamp() - 10, {"tex.png"});
    ddb::buildPending(ds.db.get(), "", false);
    EXPECT_TRUE(fs::exists(ds.pendingFile("m.obj")));
    EXPECT_FALSE(ddb::isBuildComplete(ds.db.get(), "m.obj"));

    // Attempted 120 s ago: due (the old backoff was 300 s).
    ds.backdatePending("m.obj", 120);
    ddb::buildPending(ds.db.get(), "", false);
    EXPECT_FALSE(fs::exists(ds.pendingFile("m.obj")));
    EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "m.obj"));

    // A fresh marker is retried at once with force.
    ds.writePendingMarker("m.obj", utils::currentUnixTimestamp(), {"tex.png"});
    ddb::buildPending(ds.db.get(), "", true);
    EXPECT_FALSE(fs::exists(ds.pendingFile("m.obj")));
    EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "m.obj"));
}

// P1: an attempt that finds the output already complete still consumes ".pending",
// whatever the marker lists.
TEST(PendingBuild, CompleteOutputConsumesPendingMarker) {
    Dataset ds(TEST_NAME);
    writeText(ds.path("m.obj"), objWithMtllib("mtllib m.mtl"));
    writeText(ds.path("m.mtl"), mtlWithTexture("map_Kd tex.png"));
    writePng(ds.path("tex.png"));
    ds.add({"m.obj", "m.mtl", "tex.png"});

    ddb::build(ds.db.get(), "m.obj", "", false);
    ASSERT_TRUE(ddb::isBuildComplete(ds.db.get(), "m.obj"));

    ds.writePendingMarker("m.obj", utils::currentUnixTimestamp() - 900, {"not-indexed.png"});
    ddb::buildPending(ds.db.get(), "", false);

    EXPECT_FALSE(fs::exists(ds.pendingFile("m.obj")));
    EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
    EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "m.obj"));
}

// A retry that fails for a non-dependency reason consumes the marker (the
// failure is logged; DDBBuild(pendingOnly) still succeeds).
TEST(PendingBuild, NonDependencyFailureConsumesMarker) {
    Dataset ds(TEST_NAME);
    writeText(ds.path("m.obj"), objWithMtllib("mtllib m.mtl"));
    ds.add({"m.obj"});
    expectDeferred(ds, "m.obj");

    // The MTL now references an absolute texture: unsatisfiable.
    writeText(ds.path("m.mtl"), mtlWithTexture("map_Kd /nonexistent/tex.png"));
    ds.add({"m.mtl"});
    ds.backdatePending("m.obj");

    EXPECT_EQ(DDBBuild(ds.root.string().c_str(), nullptr, nullptr, false, true), DDBERR_NONE);
    EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
    EXPECT_FALSE(ddb::isBuildComplete(ds.db.get(), "m.obj"));
}

// Only top-level "<hash>.pending" files are markers; anything else is ignored.
TEST(PendingBuild, IgnoresNonHashAndNestedPendingFiles) {
    Dataset ds(TEST_NAME);
    writeText(ds.path("m.obj"), objWithMtllib("mtllib m.mtl"));
    ds.add({"m.obj"});

    const fs::path buildDir = ds.db->buildDirectory();
    fs::create_directories(buildDir / "somefolder");
    writeText(buildDir / "notes.pending", "0\n");
    writeText(buildDir / "somefolder" / (ds.hashOf("m.obj") + ".pending"), "0\nm.mtl\n");

    ddb::buildPending(ds.db.get(), "", false);

    EXPECT_TRUE(fs::exists(buildDir / "notes.pending"));
    EXPECT_TRUE(fs::exists(buildDir / "somefolder" / (ds.hashOf("m.obj") + ".pending")));
    EXPECT_FALSE(fs::exists(ds.pendingFile("m.obj"))) << "nested marker must not be retried";
}

// ---------------------------------------------------------------------------
// Path contract (A1.2)
// ---------------------------------------------------------------------------

// mtllib mats/m.mtl + map_Kd tex.png: satisfied by mats/tex.png (MTL folder) ...
TEST(PendingBuild, MtlTextureFoundInMtlFolder) {
    Dataset ds(TEST_NAME);
    writeText(ds.path("sub/m.obj"), objWithMtllib("mtllib mats/m.mtl"));
    writeText(ds.path("sub/mats/m.mtl"), mtlWithTexture("map_Kd tex.png"));
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
    writeText(ds.path("sub/m.obj"), objWithMtllib("mtllib mats/m.mtl"));
    writeText(ds.path("sub/mats/m.mtl"), mtlWithTexture("map_Kd tex.png"));
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
    writeText(ds.path("m.obj"), objWithMtllib("mtllib mats/m.mtl"));
    writeText(ds.path("mats/m.mtl"), mtlWithTexture("map_Kd tex.png"));
    writeText(ds.path("sub/m.obj"), objWithMtllib("mtllib mats/m.mtl"));
    writeText(ds.path("sub/mats/m.mtl"), mtlWithTexture("map_Kd tex.png"));
    ds.add({"m.obj", "mats/m.mtl", "sub/m.obj", "sub/mats/m.mtl"});

    expectDeferred(ds, "m.obj");
    EXPECT_EQ(ds.pendingDeps("m.obj"), std::vector<std::string>{"mats/tex.png"});

    expectDeferred(ds, "sub/m.obj");
    EXPECT_EQ(ds.pendingDeps("sub/m.obj"), std::vector<std::string>{"sub/mats/tex.png"});
}

TEST(PendingBuild, BackslashReferencesAreNormalised) {
    Dataset ds(TEST_NAME);
    writeText(ds.path("sub/m.obj"), objWithMtllib("mtllib mats\\m.mtl"));
    writeText(ds.path("sub/mats/m.mtl"), mtlWithTexture("map_Kd tex\\a.png"));
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
    writeText(ds.path("sub/m.obj"), objWithMtllib("mtllib ../shared/m.mtl"));
    ds.add({"sub/m.obj"});

    EXPECT_EQ(getObjDependencies(ds.path("sub/m.obj").string(), ds.root.string()),
              std::vector<std::string>{"../shared/m.mtl"});

    expectDeferred(ds, "sub/m.obj");
    EXPECT_EQ(ds.pendingDeps("sub/m.obj"), std::vector<std::string>{"shared/m.mtl"});

    writeText(ds.path("shared/m.mtl"), mtlWithTexture("map_Kd ../shared/tex.png"));
    writePng(ds.path("shared/tex.png"));
    ds.add({"shared/m.mtl", "shared/tex.png"});
    ds.backdatePending("sub/m.obj");
    ddb::buildPending(ds.db.get(), "", false);

    EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
    EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "sub/m.obj"));
}

TEST(PendingBuild, AbsoluteMtllibFailsWithoutPending) {
    Dataset ds(TEST_NAME);
    writeText(ds.path("m.obj"), objWithMtllib("mtllib /nonexistent/m.mtl"));
    ds.add({"m.obj"});
    expectFailedWithoutPending(ds, "m.obj", "/nonexistent/m.mtl");
}

TEST(PendingBuild, WindowsAbsoluteTextureFailsWithoutPending) {
    Dataset ds(TEST_NAME);
    writeText(ds.path("m.obj"), objWithMtllib("mtllib m.mtl"));
    writeText(ds.path("m.mtl"), mtlWithTexture("map_Kd C:\\textures\\tex.png"));
    ds.add({"m.obj", "m.mtl"});
    expectFailedWithoutPending(ds, "m.obj", "C:\\textures\\tex.png");
}

TEST(PendingBuild, RootEscapingReferenceFailsWithoutPending) {
    Dataset ds(TEST_NAME);
    writeText(ds.path("sub/m.obj"), objWithMtllib("mtllib ../../m.mtl"));
    ds.add({"sub/m.obj"});

    // Without a dataset root the reference is just a relative path.
    EXPECT_EQ(getObjDependencies(ds.path("sub/m.obj").string()),
              std::vector<std::string>{"../../m.mtl"});
    EXPECT_THROW(getObjDependencies(ds.path("sub/m.obj").string(), ds.root.string()), AppException);

    expectFailedWithoutPending(ds, "sub/m.obj", "../../m.mtl");
}

TEST(PendingBuild, RootEscapingTextureFailsWithoutPending) {
    Dataset ds(TEST_NAME);
    writeText(ds.path("m.obj"), objWithMtllib("mtllib m.mtl"));
    writeText(ds.path("m.mtl"), mtlWithTexture("map_Kd ../outside/tex.png"));
    ds.add({"m.obj", "m.mtl"});
    expectFailedWithoutPending(ds, "m.obj", "../outside/tex.png");
}

TEST(PendingBuild, QuotedNamesWithSpaces) {
    Dataset ds(TEST_NAME);
    writeText(ds.path("sub/my model.obj"), objWithMtllib("mtllib \"my mats/my model.mtl\""));
    writeText(ds.path("sub/my mats/my model.mtl"), mtlWithTexture("map_Kd \"my tex.png\""));
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

// buildNexus (validateDependencies) reports missing names relative to the OBJ's folder.
TEST(PendingBuild, ValidateDependenciesReportsMissingFile) {
    TestArea ta(TEST_NAME, true);
    const fs::path obj = ta.getPath("m.obj");
    writeText(obj, objWithMtllib("mtllib m.mtl"));
    writeText(ta.getPath("m.mtl"), mtlWithTexture("map_Kd tex.png\nmap_bump tex.png"));

    try {
        buildNexus(obj.string(), ta.getPath("m.nxz").string(), true);
        FAIL() << "buildNexus should report the missing texture";
    } catch (const BuildDepMissingException& e) {
        EXPECT_EQ(e.getMissingDependencies(), std::vector<std::string>{"tex.png"});
    }
    EXPECT_FALSE(fs::exists(ta.getPath("m.nxz")));
}

// ---------------------------------------------------------------------------
// Shapefile sidecars (P2)
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

// Sets up sub/layer.shp indexed and deferred because sub/layer.shx is missing.
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
}

TEST(PendingBuild, ShapefileInSubfolderClearsViaBuildPending) {
    Dataset ds(TEST_NAME);
    deferShapefileInSubfolder(ds);

    ds.backdatePending("sub/layer.shp");
    ddb::buildPending(ds.db.get(), "", false);

    EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
    EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "sub/layer.shp"));
}

// The Registry builds the sidecar entry when it is indexed: that must build the
// main file in the same folder.
TEST(PendingBuild, ShapefileSidecarInSubfolderBuildsMainFile) {
    Dataset ds(TEST_NAME);
    deferShapefileInSubfolder(ds);

    std::string subfolder;
    EXPECT_TRUE(ddb::isBuildable(ds.db.get(), "sub/layer.shx", subfolder));
    EXPECT_EQ(subfolder, "vec");

    ASSERT_NO_THROW(ddb::build(ds.db.get(), "sub/layer.shx", "", false));

    EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
    EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "sub/layer.shp"));
    EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "sub/layer.shx"));
    const fs::path out = ds.db->buildDirectory() / ds.hashOf("sub/layer.shp");
    EXPECT_TRUE(fs::exists(out / "vec" / "source.gpkg"));
    EXPECT_TRUE(fs::exists(out / "mvt" / "metadata.json"));
}

TEST(PendingBuild, VectorNoSpaceRetainsExistingPendingHintsAndFreshTimestamp) {
    for (const std::string driver : {"MVT", "GPKG"})
        for (const bool onClose : {false, true}) {
            SCOPED_TRACE(driver + (onClose ? " close" : " translate"));
            Dataset ds(TEST_NAME + driver + (onClose ? "-close" : "-translate"));
            deferShapefileInSubfolder(ds); // dependency was missing, then arrived
            ds.backdatePending("sub/layer.shp");
            const auto before = utils::currentUnixTimestamp();
            {
                ScopedRetryEnv enoughDisk("DDB_MIN_FREE_DISK_MB", "0");
                ddb_test::withVectorWriteFailure(driver.c_str(), onClose, [&] {
                    EXPECT_THROW(ddb::build(ds.db.get(), "sub/layer.shp", "", false), RetryableBuildException);
                });
            }
            const auto pending = getPendingBuildInfo(ds.db.get());
            ASSERT_EQ(pending.size(), 1u);
            EXPECT_GE(pending[0].lastAttempt, before);
            EXPECT_EQ(pending[0].missingDependencies, (std::vector<std::string>{"sub/layer.shx"}));
            EXPECT_FALSE(isBuildComplete(ds.db.get(), "sub/layer.shp"));
            // The restored real driver can complete the same pending entry.
            ds.backdatePending("sub/layer.shp", 60);
            ScopedRetryEnv enoughDisk("DDB_MIN_FREE_DISK_MB", "0");
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
    ASSERT_TRUE(getEntry(ds.db.get(), "sample.splat", entry));
    ASSERT_EQ(entry.type, EntryType::GaussianSplat);
    ds.writePendingMarker("sample.splat", 1, {"previous dependency hint"});
    const auto realTool = buildlod::findBuildLodBinary(true);
    const auto before = utils::currentUnixTimestamp();
    const auto start = std::chrono::steady_clock::now();
    {
        ScopedRetryEnv tool("DDB_BUILDLOD_PATH", testChildPath().string().c_str());
        ScopedRetryEnv mode("DDB_TEST_CHILD_MODE", "idle");
        ScopedRetryEnv deadline("DDB_BUILDLOD_IDLE_TIMEOUT_SECONDS", "1");
        ScopedRetryEnv enoughDisk("DDB_MIN_FREE_DISK_MB", "0");
        buildlod::findBuildLodBinary(true);
        try {
            build(ds.db.get(), "sample.splat", "", false);
            FAIL() << "the primary tool should time out";
        } catch (const RetryableBuildException& exception) {
            EXPECT_EQ(exception.reason(), RetryableBuildReason::IdleTimeout);
        }
        EXPECT_EQ(DDBBuild(ds.root.string().c_str(), "sample.splat", nullptr, false), DDBERR_EXCEPTION);
    }
    buildlod::findBuildLodBinary(true);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(15));
    const auto pending = getPendingBuildInfo(ds.db.get());
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_GE(pending[0].lastAttempt, before);
    EXPECT_EQ(pending[0].missingDependencies, (std::vector<std::string>{"previous dependency hint"}));
    EXPECT_FALSE(isBuildComplete(ds.db.get(), "sample.splat"));
    const fs::path output = ds.db->buildDirectory() / entry.hash;
    EXPECT_FALSE(fs::exists(output / "gsplat" / "model.rad"));
    for (const auto& child : fs::directory_iterator(output))
        EXPECT_EQ(child.path().filename().string().find("gsplat-temp-"), std::string::npos);
    if (!realTool.empty()) {
        ScopedRetryEnv tool("DDB_BUILDLOD_PATH", realTool.string().c_str());
        ScopedRetryEnv enoughDisk("DDB_MIN_FREE_DISK_MB", "0");
        buildlod::findBuildLodBinary(true);
        ds.backdatePending("sample.splat", 60);
        EXPECT_NO_THROW(buildPending(ds.db.get(), "", false));
        EXPECT_FALSE(fs::exists(ds.pendingFile("sample.splat")));
        EXPECT_TRUE(isBuildComplete(ds.db.get(), "sample.splat"));
    }
    buildlod::findBuildLodBinary(true);
}

TEST(PendingBuild, InvalidBuildLodDeadlineIsPermanentAndDoesNotTouchRunnerOutputs) {
    Dataset ds(TEST_NAME);
    const fs::path input = ds.path("sample.splat");
    writeText(input, std::string(32, '\0'));
    const fs::path output = ds.path("previous.rad");
    const fs::path adjacent = ds.path("sample-lod.rad");
    for (const char* invalid : {"0", "-1", "+1", "1.5", " 1", "1 ", "abc", "86401", "9223372037", "18446744073709551616"}) {
        SCOPED_TRACE(invalid);
        writeText(output, "previous output");
        writeText(adjacent, "adjacent output");
        ScopedRetryEnv deadline("DDB_BUILDLOD_IDLE_TIMEOUT_SECONDS", invalid);
        const auto result = buildlod::runBuildLodResult(input, output, true, 1, testChildPath());
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
        ScopedRetryEnv tool("DDB_BUILDLOD_PATH", testChildPath().string().c_str());
        ScopedRetryEnv deadline("DDB_BUILDLOD_IDLE_TIMEOUT_SECONDS", "invalid");
        ScopedRetryEnv enoughDisk("DDB_MIN_FREE_DISK_MB", "0");
        buildlod::findBuildLodBinary(true);
        EXPECT_EQ(DDBBuild(ds.root.string().c_str(), "sample.splat", nullptr, false), DDBERR_EXCEPTION);
        EXPECT_FALSE(fs::exists(ds.pendingFile("sample.splat")));
    }
    buildlod::findBuildLodBinary(true);
}

TEST(PendingBuild, MaximumBuildLodDeadlineParsesAndLaunchesTool) {
    Dataset ds(TEST_NAME);
    const fs::path input = ds.path("sample.splat");
    const fs::path output = ds.path("model.rad");
    writeText(input, std::string(32, '\0'));
    ScopedRetryEnv deadline("DDB_BUILDLOD_IDLE_TIMEOUT_SECONDS", "86400");
    process::Options options;
    options.envOverrides.emplace_back("DDB_TEST_CHILD_MODE", "no-output");
    const auto started = std::chrono::steady_clock::now();
    const auto result = buildlod::runBuildLodResult(input, output, true, 1, testChildPath(), options);
    // Exit 0 with no output proves parsing accepted the upper bound and launched
    // the child. The missing-output contract still makes this a permanent failure.
    EXPECT_FALSE(result.succeeded);
    EXPECT_FALSE(result.timedOut);
    EXPECT_NE(result.error.find("reported success but expected output is missing"), std::string::npos);
    EXPECT_NO_THROW(result.throwIfRetryable("primary: "));
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(10));
}

// A pending primary failure stays visibly failed while its existing dependency
// hints and refreshed timestamp let a delayed coordinator retry the same entry.
TEST(PendingBuild, LowDiskRetainsPendingThenSucceedsAtSixtySecondBoundary) {
    Dataset ds(TEST_NAME);
    writeText(ds.path("m.obj"), objWithMtllib("mtllib m.mtl"));
    ds.add({"m.obj"});
    expectDeferred(ds, "m.obj");
    writeText(ds.path("m.mtl"), mtlWithTexture("map_Kd tex.png"));
    writePng(ds.path("tex.png"));
    ds.add({"m.mtl", "tex.png"});
    ds.backdatePending("m.obj");
    const auto beforeAttempt = utils::currentUnixTimestamp();
    {
        ScopedRetryEnv lowDisk("DDB_MIN_FREE_DISK_MB", "100000000000");
        EXPECT_THROW(ddb::build(ds.db.get(), "m.obj", "", false), RetryableBuildException);
        // The C ABI still signals an ordinary failed entry job.
        EXPECT_EQ(DDBBuild(ds.root.string().c_str(), "m.obj", nullptr, false, false), DDBERR_EXCEPTION);
    }
    const auto pending = ddb::getPendingBuildInfo(ds.db.get());
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_GE(pending[0].lastAttempt, beforeAttempt);
    EXPECT_EQ(pending[0].missingDependencies, (std::vector<std::string>{"m.mtl"}));
    EXPECT_FALSE(ds.nxzBuilt("m.obj"));

    ScopedRetryEnv enoughDisk("DDB_MIN_FREE_DISK_MB", "0");
    ddb::buildPending(ds.db.get(), "", false);
    EXPECT_TRUE(fs::exists(ds.pendingFile("m.obj"))) << "fresh attempt is inside 60-second backoff";
    ds.backdatePending("m.obj", 60);
    ddb::buildPending(ds.db.get(), "", false);
    EXPECT_FALSE(fs::exists(ds.pendingFile("m.obj")));
    EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "m.obj"));
}

TEST(PendingBuild, FreshLowDiskFailureDoesNotCreatePendingMarker) {
    Dataset ds(TEST_NAME);
    writeText(ds.path("m.obj"), objWithMtllib("mtllib missing.mtl"));
    ds.add({"m.obj"});
    ScopedRetryEnv lowDisk("DDB_MIN_FREE_DISK_MB", "100000000000");
    EXPECT_THROW(ddb::build(ds.db.get(), "m.obj", "", false), RetryableBuildException);
    EXPECT_FALSE(fs::exists(ds.pendingFile("m.obj")));
}

TEST(PendingBuild, PrimaryLockContentionLeavesMarkerByteForByteUnchanged) {
    Dataset ds(TEST_NAME);
    writeText(ds.path("m.obj"), objWithMtllib("mtllib missing.mtl"));
    ds.add({"m.obj"});
    ds.writePendingMarker("m.obj", 1, {"missing.mtl", "older hint.png"});
    const auto original = markerContents(ds.pendingFile("m.obj"));
    const auto base = ds.db->buildDirectory() / ds.hashOf("m.obj");
    fs::create_directories(base);
    BuildLock lock((base / "nxs").string());
    EXPECT_THROW(ddb::build(ds.db.get(), "m.obj", "", false), BuildInProgressException);
    EXPECT_EQ(markerContents(ds.pendingFile("m.obj")), original);
}

TEST(PendingBuild, TilesLockContentionLeavesMarkerByteForByteUnchanged) {
    Dataset ds(TEST_NAME);
    writeText(ds.path("m.obj"), objWithMtllib("mtllib missing.mtl"));
    ds.add({"m.obj"});
    ds.writePendingMarker("m.obj", 1, {"missing.mtl"});
    const auto original = markerContents(ds.pendingFile("m.obj"));
    const auto base = ds.db->buildDirectory() / ds.hashOf("m.obj");
    fs::create_directories(base);
    BuildLock lock((base / "3dtiles").string());
    EXPECT_THROW(ddb::build(ds.db.get(), "m.obj", "", false), BuildInProgressException);
    EXPECT_EQ(markerContents(ds.pendingFile("m.obj")), original);
}

#ifndef _WIN32
TEST(PendingBuild, FailedAtomicTimestampReplacementPreservesOriginalMarker) {
    Dataset ds(TEST_NAME);
    writeText(ds.path("m.obj"), objWithMtllib("mtllib missing.mtl"));
    ds.add({"m.obj"});
    ds.writePendingMarker("m.obj", 1, {"missing.mtl"});
    const auto original = markerContents(ds.pendingFile("m.obj"));
    fs::create_directories(ds.db->buildDirectory() / ds.hashOf("m.obj"));
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
    ScopedRetryEnv lowDisk("DDB_MIN_FREE_DISK_MB", "100000000000");
    EXPECT_THROW(ddb::build(ds.db.get(), "m.obj", "", false), RetryableBuildException);
    EXPECT_EQ(markerContents(ds.pendingFile("m.obj")), original);
}
#endif

}  // namespace
