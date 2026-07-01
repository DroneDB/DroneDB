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
