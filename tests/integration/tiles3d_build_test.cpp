/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// 3D Tiles integration: safe tiles rebuild (spec A3), the Model build producing
// Nexus + best-effort tiles (D-A2) and the input dispatch of buildModel3DTiles
// for PLY and glTF (B3, B5).
//
// The A3 tests drive buildModel3DTiles with a fake Obj2Tiles (a POSIX shell
// script that records its arguments and writes a tileset, or fails), so they do
// not depend on the real binary; they are skipped on Windows. Tests that need the
// real Obj2Tiles are skipped when it is not available.

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <thread>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "3d.h"
#include "build.h"
#include "buildlock.h"
#include "dbops.h"
#include "ddb.h"
#include "exceptions.h"
#include "fs.h"
#include "gtest/gtest.h"
#include "json.h"
#include "logger.h"
#include "obj2tiles_runner.h"
#include "plyconv.h"
#include "test.h"
#include "testarea.h"
#include "testfs.h"
#include "utils.h"


namespace {

using namespace ddb;

#ifdef _WIN32
constexpr const char* kBogusBinary = "Z:\\nonexistent\\Obj2Tiles_missing.exe";
#else
constexpr const char* kBogusBinary = "/nonexistent/Obj2Tiles_missing";
#endif

// Refreshes the Obj2Tiles discovery cache on entry and exit, and unsets
// DDB_OBJ2TILES_PATH on exit, so a test never leaks its binary to the next one.
struct Obj2TilesEnvReset {
    Obj2TilesEnvReset() { obj2tiles::findObj2TilesBinary(true); }
    ~Obj2TilesEnvReset() {
        unsetEnv("DDB_OBJ2TILES_PATH");
        obj2tiles::findObj2TilesBinary(true);
    }
};

void usePath(const fs::path& binary) {
    setEnv("DDB_OBJ2TILES_PATH", binary.string());
    obj2tiles::findObj2TilesBinary(true);
}

std::string readText(const fs::path& p) {
    std::ifstream in(p.string(), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::string> readLines(const fs::path& p) {
    std::ifstream in(p.string(), std::ios::binary);
    std::vector<std::string> lines;
    std::string l;
    while (std::getline(in, l))
        lines.push_back(l);
    return lines;
}

// Fake Obj2Tiles (POSIX shell). "ok" records its arguments and a copy of its
// input in the output folder and writes a tileset.json; "fail" exits non-zero.
fs::path writeFakeObj2Tiles(const fs::path& dir, bool succeed) {
    const fs::path script = dir / (succeed ? "fake_obj2tiles_ok.sh" : "fake_obj2tiles_fail.sh");
    if (succeed)
        writeFile(script,
                  "#!/bin/sh\n"
                  "out=\"$2\"\n"
                  "mkdir -p \"$out\"\n"
                  "printf '%s\\n' \"$@\" > \"$out/args.txt\"\n"
                  "cp \"$1\" \"$out/input.copy\"\n"
                  "echo '{\"asset\":{\"version\":\"1.0\"},\"geometricError\":0,\"root\":{}}' > "
                  "\"$out/tileset.json\"\n");
    else
        writeFile(script,
                  "#!/bin/sh\n"
                  "echo 'fake Obj2Tiles failure' >&2\n"
                  "exit 3\n");
    fs::permissions(script, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec,
                    fs::perm_options::replace);
    return script;
}

#ifdef _WIN32
#define SKIP_WITHOUT_POSIX_SHELL() GTEST_SKIP() << "fake Obj2Tiles is a POSIX shell script"
#else
#define SKIP_WITHOUT_POSIX_SHELL() \
    do {                           \
    } while (0)
#endif

#define SKIP_WITHOUT_OBJ2TILES()                                  \
    do {                                                          \
        if (obj2tiles::findObj2TilesBinary(true).empty())         \
            GTEST_SKIP() << "Obj2Tiles binary not available";     \
    } while (0)

// n x n grid of vertices (2 (n-1)^2 triangles) as a dependency-free OBJ.
std::string gridObj(int n = 4) {
    std::ostringstream o;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
            o << "v " << i << " " << j << " " << ((i + j) % 3) * 0.25 << "\n";
    for (int j = 0; j + 1 < n; ++j)
        for (int i = 0; i + 1 < n; ++i) {
            const int a = j * n + i + 1, b = a + 1, c = a + n + 1, d = a + n;
            o << "f " << a << " " << b << " " << c << "\nf " << a << " " << c << " " << d << "\n";
        }
    return o.str();
}

// n x n grid as an ASCII PLY mesh with uchar vertex colours (fixture F3 shape).
std::string gridPly(int n = 6) {
    std::ostringstream o;
    o << "ply\nformat ascii 1.0\ncomment synthetic F3\n"
      << "element vertex " << n * n << "\n"
      << "property float x\nproperty float y\nproperty float z\n"
      << "property uchar red\nproperty uchar green\nproperty uchar blue\n"
      << "element face " << 2 * (n - 1) * (n - 1) << "\n"
      << "property list uchar int vertex_indices\nend_header\n";
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
            o << i * 0.5 << " " << j * 0.5 << " " << std::sin(i * 0.3) << " " << (i * 40) % 256 << " "
              << (j * 40) % 256 << " " << 128 << "\n";
    for (int j = 0; j + 1 < n; ++j)
        for (int i = 0; i + 1 < n; ++i) {
            const int a = j * n + i, b = a + 1, c = a + n + 1, d = a + n;
            o << "3 " << a << " " << b << " " << c << "\n3 " << a << " " << c << " " << d << "\n";
        }
    return o.str();
}

// A glTF quad (2 triangles) with POSITION + COLOR_0 and no TEXCOORD, external .bin.
void writeNoUvGltf(const fs::path& gltf) {
    const float pos[] = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0.5f};
    const float col[] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0};
    const uint16_t idx[] = {0, 1, 2, 0, 2, 3};
    std::string bin;
    bin.append(reinterpret_cast<const char*>(pos), sizeof(pos));
    bin.append(reinterpret_cast<const char*>(col), sizeof(col));
    bin.append(reinterpret_cast<const char*>(idx), sizeof(idx));
    const fs::path binPath = fs::path(gltf).replace_extension(".bin");
    writeFile(binPath, bin);

    json j;
    j["asset"] = {{"version", "2.0"}};
    j["scene"] = 0;
    j["scenes"] = json::array({{{"nodes", json::array({0})}}});
    j["nodes"] = json::array({{{"mesh", 0}}});
    j["meshes"] = json::array(
        {{{"primitives",
           json::array({{{"attributes", {{"POSITION", 0}, {"COLOR_0", 1}}}, {"indices", 2}, {"mode", 4}}})}}});
    j["buffers"] = json::array({{{"uri", binPath.filename().string()}, {"byteLength", bin.size()}}});
    j["bufferViews"] = json::array({{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", 48}, {"target", 34962}},
                                    {{"buffer", 0}, {"byteOffset", 48}, {"byteLength", 48}, {"target", 34962}},
                                    {{"buffer", 0}, {"byteOffset", 96}, {"byteLength", 12}, {"target", 34963}}});
    j["accessors"] = json::array({{{"bufferView", 0},
                                   {"componentType", 5126},
                                   {"count", 4},
                                   {"type", "VEC3"},
                                   {"min", json::array({0, 0, 0})},
                                   {"max", json::array({1, 1, 0.5})}},
                                  {{"bufferView", 1}, {"componentType", 5126}, {"count", 4}, {"type", "VEC3"}},
                                  {{"bufferView", 2}, {"componentType", 5123}, {"count", 6}, {"type", "SCALAR"}}});
    writeFile(gltf, j.dump(2));
}

// Folder names inside dir (non-recursive).
std::vector<std::string> folderNames(const fs::path& dir) {
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.is_directory())
            names.push_back(e.path().filename().string());
    return names;
}

// True when a .b3dm under dir declares a COLOR_0 vertex attribute (glTF JSON chunk).
bool anyB3dmHasColor(const fs::path& dir) {
    for (const auto& e : fs::recursive_directory_iterator(dir))
        if (e.path().extension() == ".b3dm" && readText(e.path()).find("\"COLOR_0\"") != std::string::npos)
            return true;
    return false;
}

// No "-temp-" sibling, no "-old" sibling and no lock file left next to outDir.
void expectNoLeftovers(const fs::path& outDir) {
    const std::string name = outDir.filename().string();
    for (const auto& f : folderNames(outDir.parent_path())) {
        EXPECT_EQ(f.find(name + "-temp-"), std::string::npos) << "leftover " << f;
        EXPECT_NE(f, name + "-old") << "leftover " << f;
    }
    EXPECT_FALSE(fs::exists(outDir.string() + ".building"));
}

// ---------------------------------------------------------------------------
// A3: safe tiles rebuild
// ---------------------------------------------------------------------------

TEST(tiles3dBuild, failedRebuildKeepsPreviousTiles) {
    SKIP_WITHOUT_POSIX_SHELL();
    Obj2TilesEnvReset reset;
    TestArea ta(TEST_NAME);
    const fs::path obj = ta.getPath("model.obj");
    writeFile(obj, gridObj());
    const fs::path out = ta.getPath("3dtiles");

    usePath(writeFakeObj2Tiles(ta.getFolder(), true));
    ASSERT_EQ(buildModel3DTiles(obj.string(), out.string(), {.overwrite = true}), (out / "tileset.json").string());
    writeFile(out / "marker.txt", "previous");

    usePath(writeFakeObj2Tiles(ta.getFolder(), false));
    EXPECT_THROW(buildModel3DTiles(obj.string(), out.string(), {.overwrite = true}), Obj2TilesException);

    EXPECT_EQ(readText(out / "marker.txt"), "previous");
    EXPECT_TRUE(fs::exists(out / "tileset.json"));
    expectNoLeftovers(out);

    // Missing binary: fails fast and leaves the tiles alone too.
    usePath(kBogusBinary);
    EXPECT_THROW(buildModel3DTiles(obj.string(), out.string(), {.overwrite = true}), Obj2TilesException);
    EXPECT_EQ(readText(out / "marker.txt"), "previous");

    // Invalid input (missing MTL): the previous tiles stay as well.
    usePath(writeFakeObj2Tiles(ta.getFolder(), true));
    const fs::path broken = ta.getPath("broken.obj");
    writeFile(broken, "mtllib broken.mtl\n" + gridObj());
    EXPECT_THROW(buildModel3DTiles(broken.string(), out.string(), {.overwrite = true}), BuildDepMissingException);
    EXPECT_EQ(readText(out / "marker.txt"), "previous");
    expectNoLeftovers(out);
}

TEST(tiles3dBuild, successfulRebuildReplacesTiles) {
    SKIP_WITHOUT_POSIX_SHELL();
    Obj2TilesEnvReset reset;
    TestArea ta(TEST_NAME);
    const fs::path obj = ta.getPath("model.obj");
    writeFile(obj, gridObj());
    const fs::path out = ta.getPath("3dtiles");
    writeFile(out / "tileset.json", "{}");
    writeFile(out / "marker.txt", "previous");

    usePath(writeFakeObj2Tiles(ta.getFolder(), true));
    buildModel3DTiles(obj.string(), out.string(), {.overwrite = true});

    EXPECT_FALSE(fs::exists(out / "marker.txt"));
    EXPECT_TRUE(fs::exists(out / "args.txt"));
    expectNoLeftovers(out);

    // overwrite = false keeps refusing a non-empty output folder.
    EXPECT_THROW(buildModel3DTiles(obj.string(), out.string()), AppException);
    EXPECT_TRUE(fs::exists(out / "args.txt"));
}

TEST(tiles3dBuild, restoresOldWhenOnlyOldExists) {
    SKIP_WITHOUT_POSIX_SHELL();
    Obj2TilesEnvReset reset;
    TestArea ta(TEST_NAME);
    const fs::path obj = ta.getPath("model.obj");
    writeFile(obj, gridObj());
    const fs::path out = ta.getPath("3dtiles");
    const fs::path old = ta.getPath("3dtiles-old");
    // A crash between the two renames of the swap.
    writeFile(old / "tileset.json", "{}");
    writeFile(old / "marker.txt", "previous");

    // The restore happens under the lock before Obj2Tiles runs, so it holds even
    // when the rebuild then fails.
    usePath(writeFakeObj2Tiles(ta.getFolder(), false));
    EXPECT_THROW(buildModel3DTiles(obj.string(), out.string(), {.overwrite = true}), Obj2TilesException);

    EXPECT_EQ(readText(out / "marker.txt"), "previous");
    EXPECT_FALSE(fs::exists(old));
    expectNoLeftovers(out);
}

TEST(tiles3dBuild, deletesOldWhenBothExistThenRebuildSucceeds) {
    SKIP_WITHOUT_POSIX_SHELL();
    Obj2TilesEnvReset reset;
    TestArea ta(TEST_NAME);
    const fs::path obj = ta.getPath("model.obj");
    writeFile(obj, gridObj());
    const fs::path out = ta.getPath("3dtiles");
    const fs::path old = ta.getPath("3dtiles-old");
    // A crash before the final delete of the swap: out is the newer tileset.
    writeFile(out / "tileset.json", "{}");
    writeFile(out / "marker.txt", "newer");
    writeFile(old / "tileset.json", "{}");
    writeFile(old / "marker.txt", "older");

    usePath(writeFakeObj2Tiles(ta.getFolder(), true));
    buildModel3DTiles(obj.string(), out.string(), {.overwrite = true});

    EXPECT_FALSE(fs::exists(old));
    EXPECT_TRUE(fs::exists(out / "args.txt"));
    EXPECT_FALSE(fs::exists(out / "marker.txt"));
    expectNoLeftovers(out);
}

TEST(tiles3dBuild, bothExistAndRebuildFailsKeepsNewerTiles) {
    SKIP_WITHOUT_POSIX_SHELL();
    Obj2TilesEnvReset reset;
    TestArea ta(TEST_NAME);
    const fs::path obj = ta.getPath("model.obj");
    writeFile(obj, gridObj());
    const fs::path out = ta.getPath("3dtiles");
    const fs::path old = ta.getPath("3dtiles-old");
    writeFile(out / "tileset.json", "{}");
    writeFile(old / "tileset.json", "{}");
    writeFile(out / "marker.txt", "newer");
    writeFile(old / "marker.txt", "older");

    usePath(writeFakeObj2Tiles(ta.getFolder(), false));
    EXPECT_THROW(buildModel3DTiles(obj.string(), out.string(), {.overwrite = true}), Obj2TilesException);
    EXPECT_EQ(readText(out / "marker.txt"), "newer");
    EXPECT_FALSE(fs::exists(old));
}

TEST(tiles3dBuild, trailingSeparatorOutputDir) {
    SKIP_WITHOUT_POSIX_SHELL();
    Obj2TilesEnvReset reset;
    TestArea ta(TEST_NAME);
    const fs::path obj = ta.getPath("model.obj");
    writeFile(obj, gridObj());
    const fs::path out = ta.getPath("3dtiles");

    usePath(writeFakeObj2Tiles(ta.getFolder(), true));
    const std::string tileset = buildModel3DTiles(obj.string(), out.string() + "/", {.overwrite = true});
    EXPECT_EQ(fs::path(tileset), out / "tileset.json");
    EXPECT_TRUE(fs::exists(out / "tileset.json"));
    writeFile(out / "marker.txt", "previous");

    // The siblings are derived from the folder name, not created inside it.
    usePath(writeFakeObj2Tiles(ta.getFolder(), false));
    EXPECT_THROW(buildModel3DTiles(obj.string(), out.string() + "//", {.overwrite = true}), Obj2TilesException);
    EXPECT_EQ(readText(out / "marker.txt"), "previous");
    EXPECT_TRUE(folderNames(out).empty()) << "nothing may be created inside the output folder";
    expectNoLeftovers(out);

    usePath(writeFakeObj2Tiles(ta.getFolder(), true));
    buildModel3DTiles(obj.string(), out.string() + "/", {.overwrite = true});
    EXPECT_FALSE(fs::exists(out / "marker.txt"));
    EXPECT_TRUE(fs::exists(out / "tileset.json"));
    expectNoLeftovers(out);
}

TEST(tiles3dBuild, busyTilesLockLeavesOutputAlone) {
    SKIP_WITHOUT_POSIX_SHELL();
    Obj2TilesEnvReset reset;
    TestArea ta(TEST_NAME);
    const fs::path obj = ta.getPath("model.obj");
    writeFile(obj, gridObj());
    const fs::path out = ta.getPath("3dtiles");
    writeFile(out / "marker.txt", "previous");
    writeFile(ta.getPath("3dtiles-temp-0123456789abcdef/partial"), "in flight");

    usePath(writeFakeObj2Tiles(ta.getFolder(), true));
    {
        BuildLock other(out.string());
        EXPECT_THROW(buildModel3DTiles(obj.string(), out.string(), {.overwrite = true}), BuildInProgressException);
    }
    EXPECT_EQ(readText(out / "marker.txt"), "previous");
    EXPECT_TRUE(fs::exists(ta.getPath("3dtiles-temp-0123456789abcdef/partial")));
}

// ---------------------------------------------------------------------------
// B3: input dispatch (PLY, no-UV glTF)
// ---------------------------------------------------------------------------

// A PLY reaches Obj2Tiles as the converter's OBJ, with options computed from the
// converter's face count.
TEST(tiles3dBuild, plyIsConvertedToObjForTiles) {
    SKIP_WITHOUT_POSIX_SHELL();
    Obj2TilesEnvReset reset;
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("mesh  with  spaces.ply");
    writeFile(ply, gridPly(6));
    const fs::path out = ta.getPath("3dtiles");

    usePath(writeFakeObj2Tiles(ta.getFolder(), true));
    buildModel3DTiles(ply.string(), out.string(), {.overwrite = true});

    const auto args = readLines(out / "args.txt");
    ASSERT_GE(args.size(), 2u);
    EXPECT_EQ(fs::path(args[0]).extension(), ".obj");
    EXPECT_EQ(fs::path(args[0]).filename(), "model.obj");
    EXPECT_NE(fs::path(args[0]).parent_path(), ply.parent_path()) << "converted in a temp folder";
    EXPECT_FALSE(fs::exists(args[0])) << "the conversion temp folder is removed";

    // The OBJ carries the vertex colours (v x y z r g b) and every face.
    size_t v = 0, f = 0;
    for (const auto& l : readLines(out / "input.copy")) {
        if (l.rfind("v ", 0) == 0) {
            ++v;
            std::istringstream iss(l.substr(2));
            std::vector<double> vals;
            double d;
            while (iss >> d)
                vals.push_back(d);
            EXPECT_EQ(vals.size(), 6u) << l;
        } else if (l.rfind("f ", 0) == 0) {
            ++f;
        }
    }
    EXPECT_EQ(v, 36u);
    EXPECT_EQ(f, 50u);

    ModelInfo info;
    info.faceCount = 50;
    const auto expected = computeObj2TilesOpts(info);
    const auto argAfter = [&args](const std::string& flag) -> std::string {
        for (size_t i = 0; i + 1 < args.size(); ++i)
            if (args[i] == flag)
                return args[i + 1];
        return "";
    };
    EXPECT_EQ(argAfter("--divisions"), std::to_string(expected.divisions));
    EXPECT_EQ(argAfter("--lods"), std::to_string(expected.lods));
    expectNoLeftovers(out);
}

// B3.5: a glTF without texture coordinates becomes an OBJ (not a PLY) for the
// tiles path, and the OBJ exporter keeps its vertex colours.
TEST(tiles3dBuild, noUvGltfConvertsToColouredObj) {
    TestArea ta(TEST_NAME);
    const fs::path gltf = ta.getPath("quad.gltf");
    writeNoUvGltf(gltf);

    std::string geom, mtl;
    convertGltfTo3dModel(gltf.string(), ta.getPath("out/quad").string(), geom, mtl, false, false);
    ASSERT_EQ(fs::path(geom).extension(), ".obj");

    size_t v = 0, f = 0;
    double colourSum = 0;
    for (const auto& l : readLines(geom)) {
        if (l.rfind("v ", 0) == 0) {
            ++v;
            std::istringstream iss(l.substr(2));
            std::vector<double> vals;
            double d;
            while (iss >> d)
                vals.push_back(d);
            ASSERT_EQ(vals.size(), 6u) << "vertex colour lost: " << l;
            colourSum += vals[3] + vals[4] + vals[5];
        } else if (l.rfind("f ", 0) == 0) {
            ++f;
        }
    }
    EXPECT_GE(v, 4u);
    EXPECT_EQ(f, 2u);
    EXPECT_GT(colourSum, 0.0);

    // The Nexus path keeps the previous PLY output for a no-UV glTF.
    std::string geomPly, mtlPly;
    convertGltfTo3dModel(gltf.string(), ta.getPath("out2/quad").string(), geomPly, mtlPly, false, true);
    EXPECT_EQ(fs::path(geomPly).extension(), ".ply");
}

TEST(tiles3dBuild, noUvGltfReachesObj2TilesAsObj) {
    SKIP_WITHOUT_POSIX_SHELL();
    Obj2TilesEnvReset reset;
    TestArea ta(TEST_NAME);
    const fs::path gltf = ta.getPath("quad.gltf");
    writeNoUvGltf(gltf);
    const fs::path out = ta.getPath("3dtiles");

    usePath(writeFakeObj2Tiles(ta.getFolder(), true));
    buildModel3DTiles(gltf.string(), out.string(), {.overwrite = true});

    const auto args = readLines(out / "args.txt");
    ASSERT_FALSE(args.empty());
    EXPECT_EQ(fs::path(args[0]).extension(), ".obj");
}

// Real Obj2Tiles: tiles from a PLY and from a no-UV glTF.
TEST(tiles3dBuild, realObj2TilesFromPly) {
    Obj2TilesEnvReset reset;
    SKIP_WITHOUT_OBJ2TILES();
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("mesh.ply");
    writeFile(ply, gridPly(12));
    const fs::path out = ta.getPath("3dtiles");

    const std::string tileset = buildModel3DTiles(ply.string(), out.string(), {.overwrite = true});
    ASSERT_TRUE(fs::exists(tileset));
    bool anyB3dm = false;
    for (const auto& e : fs::recursive_directory_iterator(out))
        if (e.path().extension() == ".b3dm")
            anyB3dm = true;
    EXPECT_TRUE(anyB3dm);
    EXPECT_TRUE(anyB3dmHasColor(out)) << "PLY vertex colours must reach the tiles";
    expectNoLeftovers(out);
}

TEST(tiles3dBuild, realObj2TilesFromNoUvGltf) {
    Obj2TilesEnvReset reset;
    SKIP_WITHOUT_OBJ2TILES();
    TestArea ta(TEST_NAME);
    const fs::path gltf = ta.getPath("quad.gltf");
    writeNoUvGltf(gltf);
    const fs::path out = ta.getPath("3dtiles");

    const std::string tileset = buildModel3DTiles(gltf.string(), out.string(), {.overwrite = true});
    ASSERT_TRUE(fs::exists(tileset));
    EXPECT_TRUE(anyB3dmHasColor(out)) << "glTF vertex colours must reach the tiles";
    expectNoLeftovers(out);
}

// Heavy: the 772K-face brighton.ply through the real Obj2Tiles.
MANUAL_TEST(tiles3dBuild, realObj2TilesFromBrightonPly) {
    Obj2TilesEnvReset reset;
    SKIP_WITHOUT_OBJ2TILES();
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.downloadTestAsset(
        testDataUrl("3d/brighton.ply"), "brighton.ply");
    const fs::path out = ta.getPath("3dtiles");

    const std::string tileset = buildModel3DTiles(ply.string(), out.string(), {.overwrite = true});
    ASSERT_TRUE(fs::exists(tileset));
    expectNoLeftovers(out);
}

// ---------------------------------------------------------------------------
// D-A2: buildInternal for a Model (Nexus + best-effort tiles)
// ---------------------------------------------------------------------------

struct ModelDataset : IndexFixture {
    explicit ModelDataset(const std::string& name) : IndexFixture(name, "") {}

    void add(const std::string& rel, const std::string& content) {
        writeFile(root / rel, content);
        ddb::addToIndex(db.get(), {(root / rel).string()});
        Entry e;
        ASSERT_TRUE(loadEntry(db.get(), rel, e));
        ASSERT_EQ(e.type, EntryType::Model) << rel;
        hashes[rel] = e.hash;
    }

    fs::path out(const std::string& rel) const { return db->buildDirectory() / hashes.at(rel); }

    std::map<std::string, std::string> hashes;
};

TEST(tiles3dBuild, completeModelRecoversTilesBeforeReturningWithoutTools) {
    Obj2TilesEnvReset reset;
    usePath(kBogusBinary);
    ModelDataset ds(TEST_NAME);
    ds.add("model.obj", gridObj());
    const fs::path base = ds.out("model.obj");
    const fs::path nxs = base / "nxs" / "model.nxz";
    const fs::path tiles = base / "3dtiles";
    const fs::path old = base / "3dtiles-old";
    writeFile(nxs, "existing Nexus bytes");
    const auto nxsTime = fs::last_write_time(nxs);

    // Recovery is reached through the public API, even with no converter tool.
    for (const std::string state : {"missing", "empty", "partial", "sentinel-directory"}) {
        SCOPED_TRACE(state);
        fs::remove_all(tiles);
        writeFile(old / "tileset.json", "{}");
        writeFile(old / "marker", "usable backup");
        if (state == "empty")
            writeFile(tiles / "tileset.json", "");
        else if (state == "partial")
            writeFile(tiles / "unfinished.bin", "partial");
        else if (state == "sentinel-directory")
            fs::create_directories(tiles / "tileset.json");
        bool complete = true;
        ASSERT_EQ(DDBIsBuildComplete(ds.root.string().c_str(), "model.obj", &complete), DDBERR_NONE);
        EXPECT_FALSE(complete);
        EXPECT_TRUE(fs::exists(old / "marker")); // completeness is read-only
        ASSERT_EQ(DDBBuild(ds.root.string().c_str(), "model.obj", nullptr, false, false), DDBERR_NONE)
            << DDBGetLastError();
        EXPECT_EQ(readText(tiles / "marker"), "usable backup");
        EXPECT_FALSE(fs::exists(old));
        ASSERT_EQ(DDBIsBuildComplete(ds.root.string().c_str(), "model.obj", &complete), DDBERR_NONE);
        EXPECT_TRUE(complete);
        EXPECT_EQ(readText(nxs), "existing Nexus bytes");
        EXPECT_EQ(fs::last_write_time(nxs), nxsTime);
    }

    // A complete final wins; ordinary Nexus-only models remain complete.
    writeFile(tiles / "marker", "newer");
    writeFile(old / "tileset.json", "{}");
    ASSERT_EQ(DDBBuild(ds.root.string().c_str(), "model.obj", nullptr, false, false), DDBERR_NONE);
    EXPECT_EQ(readText(tiles / "marker"), "newer");
    EXPECT_FALSE(fs::exists(old));
    fs::remove_all(tiles);
    EXPECT_TRUE(isBuildComplete(ds.db.get(), "model.obj"));
}

TEST(tiles3dBuild, completeModelDefersRecoveryUnderLiveTilesLock) {
    ModelDataset ds(TEST_NAME);
    ds.add("model.obj", gridObj());
    const fs::path base = ds.out("model.obj");
    const fs::path tiles = base / "3dtiles";
    const fs::path old = base / "3dtiles-old";
    writeFile(base / "nxs" / "model.nxz", "existing Nexus");
    writeFile(old / "tileset.json", "{}");
    writeFile(old / "marker", "previous");
    writeFile(tiles / "unfinished", "in flight");
    const fs::path pending(base.string() + ".pending");
    writeFile(pending, "original marker\n");
    {
        BuildLock holder(tiles.string());
        EXPECT_THROW(build(ds.db.get(), "model.obj", "", false), BuildInProgressException);
        EXPECT_EQ(DDBBuild(ds.root.string().c_str(), "model.obj", nullptr, false, false), DDBERR_BUILDINPROGRESS);
        EXPECT_EQ(readText(old / "marker"), "previous");
        EXPECT_EQ(readText(tiles / "unfinished"), "in flight");
        EXPECT_EQ(readText(pending), "original marker\n");
        EXPECT_FALSE(isBuildComplete(ds.db.get(), "model.obj"));
    }
    EXPECT_NO_THROW(build(ds.db.get(), "model.obj", "", false));
    EXPECT_EQ(readText(tiles / "marker"), "previous");
    EXPECT_TRUE(isBuildComplete(ds.db.get(), "model.obj"));
}

TEST(tiles3dBuild, recoveryInspectionErrorPreservesUsableBackup) {
    TestArea ta(TEST_NAME, true);
    const fs::path tiles = ta.getFolder("3dtiles");
    const fs::path old = ta.getPath("3dtiles-old");
    writeFile(old / "tileset.json", "{}");
    writeFile(old / "marker", "last usable copy");
    std::error_code error;
    fs::create_symlink("tileset.json", tiles / "tileset.json", error);
    if (error)
        GTEST_SKIP() << "cannot create error-inducing symlink: " << error.message();
    EXPECT_THROW(recoverModel3DTilesSwap(tiles.string()), fs::filesystem_error);
    EXPECT_EQ(readText(old / "marker"), "last usable copy");
    EXPECT_EQ(readText(old / "tileset.json"), "{}");
    EXPECT_TRUE(fs::is_symlink(tiles / "tileset.json"));
    EXPECT_FALSE(fs::exists(tiles.string() + ".building"));
}

TEST(tiles3dBuild, modelBuildProducesNexusAndTiles) {
    SKIP_WITHOUT_POSIX_SHELL();
    Obj2TilesEnvReset reset;
    ModelDataset ds(TEST_NAME);
    ds.add("model.obj", gridObj());

    usePath(writeFakeObj2Tiles(ds.ta.getFolder(), true));
    ddb::build(ds.db.get(), "model.obj", "", false);

    EXPECT_TRUE(fs::exists(ds.out("model.obj") / "nxs" / "model.nxz"));
    EXPECT_TRUE(fs::exists(ds.out("model.obj") / "3dtiles" / "tileset.json"));
    EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "model.obj"));
    expectNoLeftovers(ds.out("model.obj") / "3dtiles");

    // A forced rebuild whose tiles step fails still publishes Nexus and keeps the
    // previous tiles.
    writeFile(ds.out("model.obj") / "3dtiles" / "marker.txt", "previous");
    usePath(writeFakeObj2Tiles(ds.ta.getFolder(), false));
    EXPECT_NO_THROW(ddb::build(ds.db.get(), "model.obj", "", true));
    EXPECT_TRUE(fs::exists(ds.out("model.obj") / "nxs" / "model.nxz"));
    EXPECT_EQ(readText(ds.out("model.obj") / "3dtiles" / "marker.txt"), "previous");
    expectNoLeftovers(ds.out("model.obj") / "3dtiles");
}

// Stress test for the lock order shared by builds and cleanupBuild (primary
// "nxs" lock, then "3dtiles"). A cleanup thread keeps seeding a stale
// "3dtiles-temp-*" leftover and collecting it (which takes both locks for the
// rename) while forced Model builds run on the main thread. Invariant: a build
// that returns normally has replaced the tiles (the stale marker is gone and
// tileset.json exists). Before the fix cleanup could hold the "3dtiles" lock
// alone, the build swallowed BuildInProgress as a best-effort tiles failure and
// returned normally with the old tiles still in place. A build refused on its
// primary lock is the allowed, benign outcome.
TEST(tiles3dBuild, cleanupNeverStripsTilesFromConcurrentModelBuild) {
    SKIP_WITHOUT_POSIX_SHELL();
    Obj2TilesEnvReset reset;
    ModelDataset ds(TEST_NAME);
    ds.add("model.obj", gridObj());
    usePath(writeFakeObj2Tiles(ds.ta.getFolder(), true));

    const fs::path base = ds.out("model.obj");
    const fs::path tiles = base / "3dtiles";
    ddb::build(ds.db.get(), "model.obj", "", false);
    ASSERT_TRUE(fs::exists(tiles / "tileset.json"));

    // Second connection for the cleanup thread: a Database is not shared across threads.
    auto cleanupDb = ddb::open(ds.root.string(), true);
    std::atomic<bool> stop{false};
    std::atomic<int> cleanups{0};
    std::thread cleaner([&]() {
        while (!stop.load()) {
            std::error_code ec;
            fs::create_directories(base / "3dtiles-temp-0123456789abcdef", ec);
            std::ofstream((base / "3dtiles-temp-0123456789abcdef" / "x.b3dm").string()) << "x";
            try {
                ddb::cleanupBuild(cleanupDb.get(), "");
            } catch (const std::exception&) {
                // Lock/IO contention is not what this test is about.
            }
            ++cleanups;
        }
    });

    int refused = 0, built = 0;
    for (int i = 0; i < 20; ++i) {
        writeFile(tiles / "stale.marker", "iteration " + std::to_string(i));
        try {
            ddb::build(ds.db.get(), "model.obj", "", true);
        } catch (const BuildInProgressException&) {
            // Cleanup held the primary lock: benign, the previous tiles stay intact.
            ++refused;
            EXPECT_TRUE(fs::exists(tiles / "stale.marker"));
            EXPECT_TRUE(fs::exists(tiles / "tileset.json"));
            continue;
        }
        ++built;
        EXPECT_TRUE(fs::exists(base / "nxs" / "model.nxz")) << "iteration " << i;
        EXPECT_TRUE(fs::exists(tiles / "tileset.json")) << "iteration " << i;
        EXPECT_FALSE(fs::exists(tiles / "stale.marker"))
            << "iteration " << i << ": build returned normally without replacing the tiles";
    }
    stop.store(true);
    cleaner.join();

    EXPECT_GT(built, 0);
    EXPECT_GT(cleanups.load(), 0);
    LOGD << "cleanupNeverStripsTilesFromConcurrentModelBuild: " << built << " built, " << refused
         << " refused on the primary lock, " << cleanups.load() << " cleanups";

    // Nothing is held after the run and the leftovers of the last cleanup pass
    // are collectable.
    std::error_code ec;
    fs::remove_all(base / "3dtiles-temp-0123456789abcdef", ec);
    expectNoLeftovers(tiles);
    EXPECT_NO_THROW({ BuildLock probe((base / "nxs").string(), false); });
}

TEST(tiles3dBuild, modelBuildNexusOnlyWithoutObj2Tiles) {
    Obj2TilesEnvReset reset;
    ModelDataset ds(TEST_NAME);
    ds.add("model.obj", gridObj());

    usePath(kBogusBinary);
    EXPECT_NO_THROW(ddb::build(ds.db.get(), "model.obj", "", false));

    EXPECT_TRUE(fs::exists(ds.out("model.obj") / "nxs" / "model.nxz"));
    EXPECT_FALSE(fs::exists(ds.out("model.obj") / "3dtiles"));
    EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "model.obj"));
    EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
}

TEST(tiles3dBuild, modelBuildWithRealObj2Tiles) {
    Obj2TilesEnvReset reset;
    SKIP_WITHOUT_OBJ2TILES();
    ModelDataset ds(TEST_NAME);
    ds.add("sub/mesh.ply", gridPly(8));

    ddb::build(ds.db.get(), "sub/mesh.ply", "", false);
    EXPECT_TRUE(fs::exists(ds.out("sub/mesh.ply") / "nxs" / "model.nxz"));
    EXPECT_TRUE(fs::exists(ds.out("sub/mesh.ply") / "3dtiles" / "tileset.json"));
    EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "sub/mesh.ply"));
    expectNoLeftovers(ds.out("sub/mesh.ply") / "3dtiles");
}

// An untextured PLY still goes to Nexus as is (B5) and builds through build().
TEST(tiles3dBuild, untexturedPlyBuildsNexus) {
    Obj2TilesEnvReset reset;
    ModelDataset ds(TEST_NAME);
    ds.add("mesh.ply", gridPly(6));

    usePath(kBogusBinary);
    ddb::build(ds.db.get(), "mesh.ply", "", false);
    EXPECT_TRUE(fs::exists(ds.out("mesh.ply") / "nxs" / "model.nxz"));
    EXPECT_GT(fs::file_size(ds.out("mesh.ply") / "nxs" / "model.nxz"), 0u);
    EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "mesh.ply"));
}

// ---- Spec-coverage additions (independent verification) ----

// B5: an untextured PLY is handed to Nexus as is. A PLY whose element order the
// converter forbids (face before vertex) fails inside Nexus ("Could not build
// nexus file"), not in the converter, which proves no conversion preceded Nexus.
TEST(tiles3dBuild, untexturedPlyGoesToNexusUnconverted) {
    TestArea ta(TEST_NAME, true);
    const fs::path ply = ta.getPath("mesh.ply");
    writeFile(ply,
              "ply\nformat ascii 1.0\nelement face 1\nproperty list uchar int vertex_indices\n"
              "element vertex 3\nproperty float x\nproperty float y\nproperty float z\nend_header\n"
              "3 0 1 2\n0 0 0\n1 0 0\n0 1 0\n");
    EXPECT_THROW(convertPlyToObj(ply.string(), ta.getPath("c/m").string()), AppException)
        << "precondition: the converter rejects this element order";
    try {
        buildNexus(ply.string(), ta.getPath("a.nxz").string(), false);
        FAIL() << "Nexus is expected to reject this file";
    } catch (const AppException& e) {
        EXPECT_NE(std::string(e.what()).find("Could not build nexus file"), std::string::npos) << e.what();
    }
}

// Captures log messages (POSIX narrow strings). plog cannot remove an appender,
// so a single instance lives for the whole process and is only active while a
// Scope exists.
class CaptureAppender : public plog::IAppender {
public:
    static CaptureAppender& instance() {
        static CaptureAppender* inst = [] {
            auto* a = new CaptureAppender();  // intentionally leaked: plog keeps the pointer
            plog::get()->addAppender(a);
            return a;
        }();
        return *inst;
    }
    void write(const plog::Record& r) override {
#ifndef _WIN32
        if (active_)
            messages.push_back(r.getMessage());
#endif
    }
    bool contains(const std::string& needle) const {
        for (const auto& m : messages)
            if (m.find(needle) != std::string::npos)
                return true;
        return false;
    }
    std::vector<std::string> messages;
    bool active_ = false;
};

struct CaptureScope {
    explicit CaptureScope(plog::Severity level)
        : cap(CaptureAppender::instance()), prev_(plog::get()->getMaxSeverity()) {
        cap.messages.clear();
        cap.active_ = true;
        plog::get()->setMaxSeverity(level);
    }
    ~CaptureScope() {
        cap.active_ = false;
        plog::get()->setMaxSeverity(prev_);
    }
    bool contains(const std::string& n) const { return cap.contains(n); }
    CaptureAppender& cap;
    plog::Severity prev_;
};

// B3.4: when the converter supplies face count and bounds, getModelInfo (Assimp)
// is not consulted for a PLY (it still is for an OBJ). The decision is visible in
// the debug log: "(from the converter)" vs the Assimp-based message.
TEST(tiles3dBuild, converterInfoSkipsGetModelInfo) {
    SKIP_WITHOUT_POSIX_SHELL();
    Obj2TilesEnvReset reset;
    TestArea ta(TEST_NAME);
    usePath(writeFakeObj2Tiles(ta.getFolder(), true));

    const fs::path ply = ta.getPath("mesh.ply");
    writeFile(ply, gridPly(6));
    {
        CaptureScope cap(plog::debug);
        buildModel3DTiles(ply.string(), ta.getPath("t_ply").string(), {.overwrite = true});
        EXPECT_TRUE(cap.contains("from the converter"));
        EXPECT_FALSE(cap.contains("getModelInfo failed"));
    }
    const fs::path obj = ta.getPath("mesh.obj");
    writeFile(obj, gridObj(6));
    {
        CaptureScope cap(plog::debug);
        buildModel3DTiles(obj.string(), ta.getPath("t_obj").string(), {.overwrite = true});
        EXPECT_FALSE(cap.contains("from the converter")) << "an OBJ has no converter stats";
        EXPECT_TRUE(cap.contains("K faces"));
    }
}

}  // namespace
