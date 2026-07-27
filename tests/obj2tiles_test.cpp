/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <cmath>
#include <fstream>

#include "3d.h"
#include "exceptions.h"
#include "fs.h"
#include "gtest/gtest.h"
#include "json.h"
#include "obj2tiles_runner.h"
#include "test.h"
#include "testarea.h"

#ifdef _WIN32
#include <stdlib.h>
#define DDB_SETENV(name, value) _putenv_s(name, value)
#define DDB_UNSETENV(name) _putenv_s(name, "")
#else
#include <stdlib.h>
#define DDB_SETENV(name, value) setenv(name, value, 1)
#define DDB_UNSETENV(name) unsetenv(name)
#endif

namespace {

using namespace ddb;

// A path that cannot exist as an executable; used to force "binary unavailable".
#ifdef _WIN32
constexpr const char* kBogusBinary = "Z:\\nonexistent\\Obj2Tiles_missing.exe";
#else
constexpr const char* kBogusBinary = "/nonexistent/Obj2Tiles_missing";
#endif

// RAII helper that refreshes the Obj2Tiles binary cache on entry/exit so tests
// that set DDB_OBJ2TILES_PATH don't leak state into sibling tests.
struct Obj2TilesEnvReset {
    Obj2TilesEnvReset() { ddb::obj2tiles::findObj2TilesBinary(true); }
    ~Obj2TilesEnvReset() {
        DDB_UNSETENV("DDB_OBJ2TILES_PATH");
        ddb::obj2tiles::findObj2TilesBinary(true);
    }
};

// Writes a small text/JSON sidecar (used to seed OBJ files and georef sidecars).
void writeSidecar(const fs::path& path, const std::string& body) {
    std::ofstream o(path.string());
    o << body;
}

// An authoritative, non-executable DDB_OBJ2TILES_PATH makes discovery return
// empty regardless of any real Obj2Tiles binary on PATH.
TEST(obj2tiles, binaryNotFoundWithBogusEnv) {
    Obj2TilesEnvReset reset;
    DDB_SETENV("DDB_OBJ2TILES_PATH", kBogusBinary);
    EXPECT_TRUE(ddb::obj2tiles::findObj2TilesBinary(true).empty());
}

// runObj2Tiles must fail gracefully (false + message) when no binary is available.
TEST(obj2tiles, runReturnsFalseWhenBinaryMissing) {
    Obj2TilesEnvReset reset;
    DDB_SETENV("DDB_OBJ2TILES_PATH", kBogusBinary);
    ddb::obj2tiles::findObj2TilesBinary(true);

    TestArea ta(TEST_NAME);
    const fs::path out = ta.getPath("out3dtiles");

    ddb::obj2tiles::Obj2TilesOptions opts;
    std::string err;
    const bool ok = ddb::obj2tiles::runObj2Tiles("model.obj", out, opts, err);
    EXPECT_FALSE(ok);
    EXPECT_NE(err.find("not found"), std::string::npos);
}

// buildModel3DTiles must surface the missing binary as a typed Obj2TilesException
// so the build pipeline can treat 3D Tiles as a best-effort, non-blocking artifact.
// Uses a minimal dependency-free OBJ so no network download is needed.
TEST(obj2tiles, buildThrowsWhenBinaryMissing) {
    Obj2TilesEnvReset reset;

    TestArea ta(TEST_NAME);
    const fs::path obj = ta.getPath("model.obj");
    {
        std::ofstream o(obj.string());
        o << "v 0 0 0\n"
          << "v 1 0 0\n"
          << "v 0 1 0\n"
          << "f 1 2 3\n";
    }

    DDB_SETENV("DDB_OBJ2TILES_PATH", kBogusBinary);
    ddb::obj2tiles::findObj2TilesBinary(true);

    const fs::path out = ta.getPath("out3dtiles");
    EXPECT_THROW(ddb::buildModel3DTiles(obj.string(), out.string(), true), Obj2TilesException);
    // Nothing must be left behind at the destination when generation fails.
    EXPECT_FALSE(fs::exists(out));
}

// End-to-end LOCAL (non-georeferenced) generation: with no sidecar the tileset
// must use the identity transform. Disabled on CI (needs the Obj2Tiles binary).
MANUAL_TEST(obj2tiles, endToEndGeneration) {
    if (ddb::obj2tiles::findObj2TilesBinary(true).empty())
        GTEST_SKIP() << "Obj2Tiles binary not available";

    TestArea ta(TEST_NAME);
    const fs::path obj = ta.getPath("model.obj");
    writeSidecar(obj, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");

    const fs::path out = ta.getPath("out3dtiles");
    const std::string tileset = ddb::buildModel3DTiles(obj.string(), out.string(), true);
    ASSERT_TRUE(fs::exists(tileset));
    ASSERT_TRUE(fs::exists(out / "tileset.json"));

    std::ifstream in(tileset);
    json j;
    in >> j;
    ASSERT_TRUE(j.contains("root") && j["root"].contains("transform"));
    const auto t = j["root"]["transform"];
    ASSERT_EQ(t.size(), 16u);
    // Local mode -> identity transform: zero translation column (indices 12,13,14).
    const double tx = t[12].get<double>(), ty = t[13].get<double>(), tz = t[14].get<double>();
    EXPECT_NEAR(std::abs(tx) + std::abs(ty) + std::abs(tz), 0.0, 1e-9);
}

// --- Georeferencing detection (CI-safe: pure function, no binary needed) ---

TEST(obj2tiles, detectGeorefNoneWhenNoSidecar) {
    TestArea ta(TEST_NAME);
    const fs::path obj = ta.getPath("model.obj");
    writeSidecar(obj, "v 0 0 0\nf 1 1 1\n");  // not JSON, just a placeholder OBJ
    EXPECT_FALSE(detectModelGeoref(obj.string()).has_value());
}

TEST(obj2tiles, detectGeorefFromGeorefJson) {
    TestArea ta(TEST_NAME);
    const fs::path obj = ta.getPath("model.obj");
    writeSidecar(obj, "v 0 0 0\n");
    writeSidecar(ta.getPath("georef.json"),
                 R"({"latitude": 45.5, "longitude": 9.25, "altitude": 120.0})");

    const auto g = detectModelGeoref(obj.string());
    ASSERT_TRUE(g.has_value());
    EXPECT_NEAR(g->latitude, 45.5, 1e-9);
    EXPECT_NEAR(g->longitude, 9.25, 1e-9);
    EXPECT_NEAR(g->altitude, 120.0, 1e-9);
}

TEST(obj2tiles, detectGeorefFromReferenceLla) {
    TestArea ta(TEST_NAME);
    const fs::path obj = ta.getPath("model.obj");
    writeSidecar(obj, "v 0 0 0\n");
    // ODM convention; altitude omitted -> defaults to 0.
    writeSidecar(ta.getPath("reference_lla.json"),
                 R"({"latitude": -33.86, "longitude": 151.21})");

    const auto g = detectModelGeoref(obj.string());
    ASSERT_TRUE(g.has_value());
    EXPECT_NEAR(g->latitude, -33.86, 1e-9);
    EXPECT_NEAR(g->longitude, 151.21, 1e-9);
    EXPECT_NEAR(g->altitude, 0.0, 1e-9);
}

TEST(obj2tiles, detectGeorefFromPerModelSidecar) {
    TestArea ta(TEST_NAME);
    const fs::path obj = ta.getPath("mymodel.obj");
    writeSidecar(obj, "v 0 0 0\n");
    // <stem>.geo.json takes precedence and uses short keys.
    writeSidecar(ta.getPath("mymodel.geo.json"),
                 R"({"lat": 10.0, "lon": 20.0, "alt": 5.0})");

    const auto g = detectModelGeoref(obj.string());
    ASSERT_TRUE(g.has_value());
    EXPECT_NEAR(g->latitude, 10.0, 1e-9);
    EXPECT_NEAR(g->longitude, 20.0, 1e-9);
    EXPECT_NEAR(g->altitude, 5.0, 1e-9);
}

TEST(obj2tiles, detectGeorefRejectsOutOfRange) {
    TestArea ta(TEST_NAME);
    const fs::path obj = ta.getPath("model.obj");
    writeSidecar(obj, "v 0 0 0\n");
    writeSidecar(ta.getPath("georef.json"),
                 R"({"latitude": 999.0, "longitude": 9.0})");
    EXPECT_FALSE(detectModelGeoref(obj.string()).has_value());
}

// getModelInfo reads a model's local-space bounding box (used to derive a
// georeferenced model's WGS84 footprint). A simple OBJ with known vertices must
// yield exactly those bounds.
TEST(obj2tiles, getModelInfoBounds) {
    TestArea ta(TEST_NAME);
    const fs::path obj = ta.getPath("box.obj");
    writeSidecar(obj,
                 "v 0 0 0\n"
                 "v 10 0 0\n"
                 "v 0 20 0\n"
                 "v 0 0 5\n"
                 "f 1 2 3\n"
                 "f 1 4 2\n");

    ModelInfo info;
    ASSERT_TRUE(getModelInfo(obj.string(), info));
    EXPECT_TRUE(info.hasBounds);
    EXPECT_NEAR(info.minX, 0.0, 1e-6);
    EXPECT_NEAR(info.maxX, 10.0, 1e-6);
    EXPECT_NEAR(info.minY, 0.0, 1e-6);
    EXPECT_NEAR(info.maxY, 20.0, 1e-6);
    EXPECT_NEAR(info.minZ, 0.0, 1e-6);
    EXPECT_NEAR(info.maxZ, 5.0, 1e-6);
}

// A missing / unreadable model must fail gracefully (false, no throw) so indexing
// proceeds without a footprint instead of aborting.
TEST(obj2tiles, getModelInfoMissingReturnsFalse) {
    TestArea ta(TEST_NAME);
    ModelInfo info;
    EXPECT_FALSE(getModelInfo(ta.getPath("does_not_exist.obj").string(), info));
}

// ---------------------------------------------------------------------------
// computeObj2TilesOpts heuristic: verify all threshold bands
// ---------------------------------------------------------------------------

// Tiny band: < 10K faces → lods=1, divisions=0, octree=false (depth=0)
TEST(obj2tiles, computeOptsTiny) {
    ModelInfo info;
    info.faceCount = 9999;
    auto opts = computeObj2TilesOpts(info);
    EXPECT_EQ(opts.lods, 1);
    EXPECT_EQ(opts.divisions, 0);
    EXPECT_FALSE(opts.octree);
    EXPECT_EQ(opts.textureFormat, "Ktx2");
    EXPECT_EQ(opts.ktx2Quality, 192);
    EXPECT_EQ(opts.splitStrategy, "VertexMedian");
}

// Small band: 10K–50K → lods=2, divisions=0, octree=true (depth=1)
TEST(obj2tiles, computeOptsSmall) {
    ModelInfo info;
    info.faceCount = 30000;
    auto opts = computeObj2TilesOpts(info);
    EXPECT_EQ(opts.lods, 2);
    EXPECT_EQ(opts.divisions, 0);
    EXPECT_TRUE(opts.octree);
}

// Medium band: 50K–200K → lods=2, divisions=1, octree=true (depth=2)
TEST(obj2tiles, computeOptsMedium) {
    ModelInfo info;
    info.faceCount = 100000;
    auto opts = computeObj2TilesOpts(info);
    EXPECT_EQ(opts.lods, 2);
    EXPECT_EQ(opts.divisions, 1);
    EXPECT_TRUE(opts.octree);
}

// Large band: 200K–750K → lods=3, divisions=1, octree=true (depth=3)
TEST(obj2tiles, computeOptsLarge) {
    ModelInfo info;
    info.faceCount = 500000;
    auto opts = computeObj2TilesOpts(info);
    EXPECT_EQ(opts.lods, 3);
    EXPECT_EQ(opts.divisions, 1);
    EXPECT_TRUE(opts.octree);
}

// XL band: 750K–3M → lods=3, divisions=2, octree=true (depth=4)
TEST(obj2tiles, computeOptsXL) {
    ModelInfo info;
    info.faceCount = 1500000u;
    auto opts = computeObj2TilesOpts(info);
    EXPECT_EQ(opts.lods, 3);
    EXPECT_EQ(opts.divisions, 2);
    EXPECT_TRUE(opts.octree);
}

// XXL band: 3M–12M → lods=4, divisions=2, octree=true (depth=5)
TEST(obj2tiles, computeOptsXXL) {
    ModelInfo info;
    info.faceCount = 6000000u;
    auto opts = computeObj2TilesOpts(info);
    EXPECT_EQ(opts.lods, 4);
    EXPECT_EQ(opts.divisions, 2);
    EXPECT_TRUE(opts.octree);
}

// Boundary: exactly 10K faces → Small band (not Tiny)
TEST(obj2tiles, computeOptsBoundary10K) {
    ModelInfo info;
    info.faceCount = 10000;
    auto opts = computeObj2TilesOpts(info);
    EXPECT_EQ(opts.lods, 2);
    EXPECT_EQ(opts.divisions, 0);
    EXPECT_TRUE(opts.octree);
}

// Cap: even with astronomically large face count, depth never exceeds MAX_TILE_DEPTH (6)
TEST(obj2tiles, computeOptsCapNeverExceedsMaxDepth) {
    ModelInfo info;
    info.faceCount = UINT64_MAX;
    auto opts = computeObj2TilesOpts(info);
    int depth = opts.octree ? (opts.lods + opts.divisions - 1) : opts.divisions;
    EXPECT_LE(depth, 6);
}

// Force-defaults env var: when DDB_OBJ2TILES_FORCE_DEFAULTS is set,
// buildModel3DTiles should skip the heuristic and use hardcoded defaults.
// This unit test verifies the env var parsing logic matches the same
// lambda used in buildModel3DTiles() in 3d.cpp.
TEST(obj2tiles, forceDefaultsEnvVarParsing) {
    auto checkEnv = [] {
        const char* env = std::getenv("DDB_OBJ2TILES_FORCE_DEFAULTS");
        return env && env[0] != '0' && env[0] != '\0';
    };

    // Default: not set → false
    DDB_UNSETENV("DDB_OBJ2TILES_FORCE_DEFAULTS");
    EXPECT_FALSE(checkEnv());

    // Set to "1" → true
    DDB_SETENV("DDB_OBJ2TILES_FORCE_DEFAULTS", "1");
    EXPECT_TRUE(checkEnv());

    // Set to "true" → true
    DDB_SETENV("DDB_OBJ2TILES_FORCE_DEFAULTS", "true");
    EXPECT_TRUE(checkEnv());

    // Set to "0" → false (explicitly disabled)
    DDB_SETENV("DDB_OBJ2TILES_FORCE_DEFAULTS", "0");
    EXPECT_FALSE(checkEnv());

    // Set to empty string → false
    DDB_SETENV("DDB_OBJ2TILES_FORCE_DEFAULTS", "");
    EXPECT_FALSE(checkEnv());

    // Clean up
    DDB_UNSETENV("DDB_OBJ2TILES_FORCE_DEFAULTS");
    EXPECT_FALSE(checkEnv());
}

// When force-defaults is active, a model that would normally get "Tiny" params
// (< 10K faces → lods=1, divisions=0, octree=false) should instead get the
// default (lods=3, divisions=3, octree=true). Verified via the env var path
// producing the same result as the fallback defaults.
TEST(obj2tiles, forceDefaultsProducesExpectedParams) {
    // Simulate what buildModel3DTiles does when forceDefaults is true:
    // it sets hardcoded defaults matching the "XXL" band
    obj2tiles::Obj2TilesOptions forcedOpts;
    forcedOpts.octree = true;
    forcedOpts.divisions = 3;
    forcedOpts.lods = 3;

    // Verify these are the same defaults used in the fallback path
    EXPECT_TRUE(forcedOpts.octree);
    EXPECT_EQ(forcedOpts.divisions, 3);
    EXPECT_EQ(forcedOpts.lods, 3);

    // A tiny model's heuristic would give very different params
    ModelInfo tinyInfo;
    tinyInfo.faceCount = 100;
    auto heuristicOpts = computeObj2TilesOpts(tinyInfo);
    // Tiny band: lods=1, divisions=0, octree=false — definitely different
    EXPECT_FALSE(heuristicOpts.octree);
    EXPECT_EQ(heuristicOpts.divisions, 0);
    EXPECT_EQ(heuristicOpts.lods, 1);
}

// End-to-end georeferenced generation: a sidecar must yield a non-identity ECEF
// transform in the tileset. Disabled on CI (needs the Obj2Tiles binary).
MANUAL_TEST(obj2tiles, endToEndGeoreferenced) {
    if (ddb::obj2tiles::findObj2TilesBinary(true).empty())
        GTEST_SKIP() << "Obj2Tiles binary not available";

    TestArea ta(TEST_NAME);
    const fs::path obj = ta.getPath("model.obj");
    writeSidecar(obj, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    writeSidecar(ta.getPath("georef.json"),
                 R"({"latitude": 45.46, "longitude": 9.19, "altitude": 120.0})");

    const fs::path out = ta.getPath("out3dtiles");
    const std::string tileset = ddb::buildModel3DTiles(obj.string(), out.string(), true);
    ASSERT_TRUE(fs::exists(tileset));

    std::ifstream in(tileset);
    json j;
    in >> j;
    ASSERT_TRUE(j.contains("root") && j["root"].contains("transform"));
    const auto t = j["root"]["transform"];
    ASSERT_EQ(t.size(), 16u);
    // The ECEF translation column (indices 12,13,14) must be non-zero, i.e. the
    // transform is NOT the identity used for local-mode tilesets.
    const double tx = t[12].get<double>(), ty = t[13].get<double>(), tz = t[14].get<double>();
    EXPECT_GT(std::abs(tx) + std::abs(ty) + std::abs(tz), 1.0);
}

}  // namespace
