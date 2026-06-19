/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "build.h"
#include "dbops.h"
#include "ddb.h"
#include "entry.h"
#include "exceptions.h"
#include "gsplat.h"
#include "buildlod_runner.h"
#include "gtest/gtest.h"
#include "ply.h"
#include "rad.h"
#include "test.h"
#include "testarea.h"
#include "thumbs.h"

namespace {

using namespace ddb;

// ---------------------------------------------------------------------------
// Local fixture helpers - generate deterministic splat data without any network
// dependency, so detection/build/thumbnail behaviour is exercised in isolation.
// ---------------------------------------------------------------------------

int fRestCountForDegree(int shDegree) {
    switch (shDegree) {
        case 1: return 9;
        case 2: return 24;
        case 3: return 45;
        default: return 0;
    }
}

// Write a minimal but valid binary-little-endian 3DGS PLY that SPZ can decode.
void writeSplatPly(const fs::path& path, int n, int shDegree) {
    const int fRest = fRestCountForDegree(shDegree);

    std::vector<std::string> props = {"x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2"};
    for (int i = 0; i < fRest; ++i)
        props.push_back("f_rest_" + std::to_string(i));
    props.push_back("opacity");
    props.push_back("scale_0");
    props.push_back("scale_1");
    props.push_back("scale_2");
    props.push_back("rot_0");
    props.push_back("rot_1");
    props.push_back("rot_2");
    props.push_back("rot_3");

    std::ofstream out(path.string(), std::ios::binary);
    out << "ply\n";
    out << "format binary_little_endian 1.0\n";
    out << "element vertex " << n << "\n";
    for (const auto& p : props)
        out << "property float " << p << "\n";
    out << "end_header\n";

    const int rotBase = 6 + fRest + 1 + 3; // x,y,z + f_dc(3) + f_rest + opacity + scale(3)
    for (int i = 0; i < n; ++i) {
        std::vector<float> row(props.size(), 0.0f);
        row[0] = static_cast<float>(i % 10);          // x
        row[1] = static_cast<float>(i / 10);          // y
        row[2] = static_cast<float>(i % 3) * 0.1f;    // z (flat -> top-down)
        row[3] = 0.2f;                                // f_dc_0
        row[4] = 0.1f;                                // f_dc_1
        row[5] = -0.1f;                               // f_dc_2
        const int opacityIdx = 6 + fRest;
        row[opacityIdx] = 5.0f;                       // opacity logit (opaque)
        row[opacityIdx + 1] = -3.0f;                  // scale_0 (log)
        row[opacityIdx + 2] = -3.0f;                  // scale_1
        row[opacityIdx + 3] = -3.0f;                  // scale_2
        row[rotBase + 0] = 1.0f;                      // rot_0 = w (identity)
        out.write(reinterpret_cast<const char*>(row.data()),
                  static_cast<std::streamsize>(row.size() * sizeof(float)));
    }
}

// Write a plain point-cloud PLY (no splat attributes, no faces).
void writePointCloudPly(const fs::path& path, int n) {
    std::ofstream out(path.string(), std::ios::binary);
    out << "ply\n";
    out << "format binary_little_endian 1.0\n";
    out << "element vertex " << n << "\n";
    out << "property float x\n";
    out << "property float y\n";
    out << "property float z\n";
    out << "property uchar red\n";
    out << "property uchar green\n";
    out << "property uchar blue\n";
    out << "end_header\n";
    for (int i = 0; i < n; ++i) {
        float xyz[3] = {static_cast<float>(i), 0.0f, 0.0f};
        uint8_t rgb[3] = {10, 20, 30};
        out.write(reinterpret_cast<const char*>(xyz), sizeof(xyz));
        out.write(reinterpret_cast<const char*>(rgb), sizeof(rgb));
    }
}

// Write a tiny ASCII mesh PLY (has an element face).
void writeMeshPly(const fs::path& path) {
    std::ofstream out(path.string());
    out << "ply\n";
    out << "format ascii 1.0\n";
    out << "element vertex 3\n";
    out << "property float x\n";
    out << "property float y\n";
    out << "property float z\n";
    out << "element face 1\n";
    out << "property list uchar int vertex_indices\n";
    out << "end_header\n";
    out << "0 0 0\n1 0 0\n0 1 0\n";
    out << "3 0 1 2\n";
}

// Write an antimatter15 .splat binary (32 bytes/primitive).
void writeSplatBinary(const fs::path& path, int n) {
    std::ofstream out(path.string(), std::ios::binary);
    for (int i = 0; i < n; ++i) {
        float pos[3] = {static_cast<float>(i % 10), static_cast<float>(i / 10), 0.0f};
        float scale[3] = {0.01f, 0.01f, 0.01f}; // linear
        uint8_t rgba[4] = {128, 130, 132, 255};
        uint8_t rot[4] = {255, 128, 128, 128}; // w~1, x=y=z=0
        out.write(reinterpret_cast<const char*>(pos), sizeof(pos));
        out.write(reinterpret_cast<const char*>(scale), sizeof(scale));
        out.write(reinterpret_cast<const char*>(rgba), sizeof(rgba));
        out.write(reinterpret_cast<const char*>(rot), sizeof(rot));
    }
}

bool isWebp(const fs::path& path) {
    std::ifstream in(path.string(), std::ios::binary);
    if (!in.good()) return false;
    char hdr[12] = {0};
    in.read(hdr, sizeof(hdr));
    if (in.gcount() < 12) return false;
    return std::string(hdr, 4) == "RIFF" && std::string(hdr + 8, 4) == "WEBP";
}

std::uintmax_t fileSize(const fs::path& p) {
    std::error_code ec;
    const auto s = fs::file_size(p, ec);
    return ec ? 0 : s;
}

// True when the file starts with the gzip magic (0x1F 0x8B), i.e. a legacy SPZ v1-3.
// The Spark web viewer only decodes gzip-based SPZ, so the delivery artifact must be gzip.
bool isGzip(const fs::path& p) {
    std::ifstream in(p.string(), std::ios::binary);
    if (!in.good()) return false;
    unsigned char m[2] = {0};
    in.read(reinterpret_cast<char*>(m), 2);
    return in.gcount() == 2 && m[0] == 0x1F && m[1] == 0x8B;
}

// True when the file starts with the Spark RAD container magic ("RAD0" = 0x52 0x41 0x44 0x30).
bool isRad(const fs::path& p) {
    std::ifstream in(p.string(), std::ios::binary);
    if (!in.good()) return false;
    char m[4] = {0};
    in.read(m, 4);
    return in.gcount() == 4 && m[0] == 'R' && m[1] == 'A' && m[2] == 'D' && m[3] == '0';
}

// ---------------------------------------------------------------------------
// Detection
// ---------------------------------------------------------------------------

TEST(gsplat, detectSplatPly) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("splat.ply");
    writeSplatPly(ply, 50, 1);

    EXPECT_EQ(ddb::fingerprint(ply), EntryType::GaussianSplat);

    PlyInfo info;
    ASSERT_TRUE(ddb::getPlyInfo(ply, info));
    EXPECT_TRUE(info.isSplat);
    EXPECT_FALSE(info.isMesh);
    EXPECT_EQ(info.shDegree, 1);
    EXPECT_EQ(info.vertexCount, 50u);
}

TEST(gsplat, plainPointCloudPlyIsNotSplat) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("cloud.ply");
    writePointCloudPly(ply, 20);

    EXPECT_EQ(ddb::fingerprint(ply), EntryType::PointCloud);

    PlyInfo info;
    ASSERT_TRUE(ddb::getPlyInfo(ply, info));
    EXPECT_FALSE(info.isSplat);
    EXPECT_EQ(info.shDegree, -1);
}

TEST(gsplat, meshPlyIsNotSplat) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("mesh.ply");
    writeMeshPly(ply);

    EXPECT_EQ(ddb::fingerprint(ply), EntryType::Model);

    PlyInfo info;
    ASSERT_TRUE(ddb::getPlyInfo(ply, info));
    EXPECT_TRUE(info.isMesh);
    EXPECT_FALSE(info.isSplat);
}

TEST(gsplat, detectByExtension) {
    TestArea ta(TEST_NAME);

    const fs::path splat = ta.getPath("a.splat");
    writeSplatBinary(splat, 4);
    EXPECT_EQ(ddb::fingerprint(splat), EntryType::GaussianSplat);

    const fs::path spz = ta.getPath("a.spz");
    {
        std::ofstream o(spz.string(), std::ios::binary);
        const char magic[4] = {'N', 'G', 'S', 'P'};
        o.write(magic, sizeof(magic));
    }
    EXPECT_EQ(ddb::fingerprint(spz), EntryType::GaussianSplat);

    const fs::path ksplat = ta.getPath("a.ksplat");
    { std::ofstream o(ksplat.string(), std::ios::binary); o << "anything"; }
    EXPECT_EQ(ddb::fingerprint(ksplat), EntryType::GaussianSplat);
}

TEST(gsplat, shDegreeFromFRestCount) {
    TestArea ta(TEST_NAME);
    const int degrees[] = {0, 1, 2, 3};
    for (int d : degrees) {
        const fs::path ply = ta.getPath("deg" + std::to_string(d) + ".ply");
        writeSplatPly(ply, 8, d);
        PlyInfo info;
        ASSERT_TRUE(ddb::getPlyInfo(ply, info)) << "degree " << d;
        EXPECT_TRUE(info.isSplat) << "degree " << d;
        EXPECT_EQ(info.shDegree, d) << "degree " << d;
    }
}

TEST(gsplat, looksLikeHelpers) {
    TestArea ta(TEST_NAME);

    const fs::path spz = ta.getPath("ok.spz");
    {
        std::ofstream o(spz.string(), std::ios::binary);
        const char magic[4] = {'N', 'G', 'S', 'P'};
        o.write(magic, sizeof(magic));
    }
    EXPECT_TRUE(ddb::looksLikeSpz(spz));

    const fs::path notSpz = ta.getPath("bad.spz");
    { std::ofstream o(notSpz.string(), std::ios::binary); o << "junk"; }
    EXPECT_FALSE(ddb::looksLikeSpz(notSpz));

    const fs::path splat = ta.getPath("ok.splat");
    writeSplatBinary(splat, 3);
    EXPECT_TRUE(ddb::looksLikeSplatBinary(splat));

    const fs::path badSplat = ta.getPath("bad.splat");
    { std::ofstream o(badSplat.string(), std::ios::binary); o << "12345"; } // 5 bytes
    EXPECT_FALSE(ddb::looksLikeSplatBinary(badSplat));
}

TEST(gsplat, getInfoPlyAndSplat) {
    TestArea ta(TEST_NAME);

    const fs::path ply = ta.getPath("info.ply");
    writeSplatPly(ply, 42, 2);
    GaussianSplatInfo plyInfo;
    ASSERT_TRUE(ddb::getGaussianSplatInfo(ply.string(), plyInfo));
    EXPECT_EQ(plyInfo.splatCount, 42u);
    EXPECT_EQ(plyInfo.shDegree, 2);

    const fs::path splat = ta.getPath("info.splat");
    writeSplatBinary(splat, 7);
    GaussianSplatInfo splatInfo;
    ASSERT_TRUE(ddb::getGaussianSplatInfo(splat.string(), splatInfo));
    EXPECT_EQ(splatInfo.splatCount, 7u);
    EXPECT_EQ(splatInfo.shDegree, 0);
}

// ---------------------------------------------------------------------------
// Conversion / build
// ---------------------------------------------------------------------------

TEST(gsplat, convertPlyToSpz) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("scene.ply");
    writeSplatPly(ply, 100, 1);

    const fs::path spz = ta.getPath("scene.spz");
    ddb::convertToSpz(ply.string(), spz.string());

    ASSERT_TRUE(fs::exists(spz));
    EXPECT_GT(fileSize(spz), 0u);
    EXPECT_TRUE(ddb::looksLikeSpz(spz));
    // The delivery artifact must be gzip-based SPZ (v3) for the Spark viewer.
    EXPECT_TRUE(isGzip(spz)) << "convertToSpz must emit gzip-based SPZ v3, not NGSP/ZSTD v4";

    GaussianSplatInfo info;
    ASSERT_TRUE(ddb::getGaussianSplatInfo(spz.string(), info));
    EXPECT_EQ(info.splatCount, 100u);
    EXPECT_EQ(info.shDegree, 1);
}

TEST(gsplat, convertSplatBinaryToSpz) {
    TestArea ta(TEST_NAME);
    const fs::path splat = ta.getPath("scene.splat");
    writeSplatBinary(splat, 64);

    const fs::path spz = ta.getPath("scene.spz");
    ddb::convertToSpz(splat.string(), spz.string());

    ASSERT_TRUE(fs::exists(spz));
    EXPECT_TRUE(ddb::looksLikeSpz(spz));

    GaussianSplatInfo info;
    ASSERT_TRUE(ddb::getGaussianSplatInfo(spz.string(), info));
    EXPECT_EQ(info.splatCount, 64u);
}

TEST(gsplat, convertSpzCopiesAsIs) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("scene.ply");
    writeSplatPly(ply, 30, 0);
    const fs::path spz = ta.getPath("scene.spz");
    ddb::convertToSpz(ply.string(), spz.string());

    const fs::path copy = ta.getPath("copy.spz");
    ddb::convertToSpz(spz.string(), copy.string());
    ASSERT_TRUE(fs::exists(copy));
    EXPECT_EQ(fileSize(copy), fileSize(spz));
}

// A gzip-based SPZ (v3) produced by convertToSpz is copied through unchanged, and the
// result remains gzip (viewer-compatible). This guards the transcode-vs-copy branch.
TEST(gsplat, deliverySpzIsAlwaysGzip) {
    TestArea ta(TEST_NAME);
    const fs::path splat = ta.getPath("scene.splat");
    writeSplatBinary(splat, 32);
    const fs::path spz = ta.getPath("scene.spz");
    ddb::convertToSpz(splat.string(), spz.string());
    ASSERT_TRUE(fs::exists(spz));
    EXPECT_TRUE(isGzip(spz)) << ".splat -> .spz must produce gzip-based SPZ v3";

    // Re-converting the gzip .spz is a copy and stays gzip.
    const fs::path copy = ta.getPath("copy.spz");
    ddb::convertToSpz(spz.string(), copy.string());
    EXPECT_TRUE(isGzip(copy));
}

TEST(gsplat, buildOutputsArtifacts) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("scene.ply");
    writeSplatPly(ply, 80, 1);

    // Reflect the real discovery state (a sibling test may have pinned the override).
    ddb::buildlod::findBuildLodBinary(/*forceRefresh=*/true);

    const fs::path outdir = ta.getFolder("gsplat_out");

    if (!ddb::buildlod::isBuildLodAvailable()) {
        // build-lod is a mandatory dependency: without it the build is deferred (the missing
        // dependency is surfaced) and no artifacts are produced.
        EXPECT_THROW(ddb::buildGsplat(ply.string(), outdir.string()), BuildDepMissingException);
        GTEST_SKIP() << "build-lod not discoverable; skipping artifact assertions";
    }

    ddb::buildGsplat(ply.string(), outdir.string());

    // model.rad is the sole delivery artifact - no intermediate .spz, no pre-rendered thumbnail.
    const fs::path modelRad = outdir / GsplatRadFileName;
    ASSERT_TRUE(fs::exists(modelRad)) << "model.rad must be produced";
    EXPECT_GT(fileSize(modelRad), 0u);
    EXPECT_TRUE(isRad(modelRad)) << "model.rad must start with the RAD0 magic";
    EXPECT_FALSE(fs::exists(outdir / GsplatFileName)) << "no intermediate model.spz is kept";
    EXPECT_FALSE(fs::exists(outdir / GsplatThumbFileName))
        << "thumbnails are rendered on demand from the .rad, not at build time";

    // No georef supplied -> no sidecar.
    EXPECT_FALSE(fs::exists(outdir / GsplatGeorefFileName));

    // A bounds sidecar is always written for deterministic camera framing in the viewer.
    const fs::path bounds = outdir / GsplatBoundsFileName;
    ASSERT_TRUE(fs::exists(bounds)) << "buildGsplat must write bounds.json";
    EXPECT_GT(fileSize(bounds), 0u);
    std::ifstream bin(bounds.string(), std::ios::binary);
    const json b = json::parse(bin);
    ASSERT_TRUE(b.contains("min") && b.contains("max"));
    EXPECT_EQ(b["min"].size(), 3u);
    EXPECT_EQ(b["max"].size(), 3u);
    for (int k = 0; k < 3; ++k)
        EXPECT_LE(b["min"][k].get<double>(), b["max"][k].get<double>());
}

// The RAD reader decodes the coarse first chunk for on-the-fly previews, and generateThumb
// dispatches a .rad to the splat rasteriser - the same on-demand model used for COPC point
// clouds. No build-time thumbnail or intermediate .spz is produced.
TEST(gsplat, radReaderAndOnTheFlyThumbnail) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("scene.ply");
    writeSplatPly(ply, 4000, 3); // enough splats for a non-trivial LOD tree, full SH

    // Reflect the real discovery state (a sibling test may have pinned the override).
    ddb::buildlod::findBuildLodBinary(/*forceRefresh=*/true);
    if (!ddb::buildlod::isBuildLodAvailable())
        GTEST_SKIP() << "build-lod not discoverable; skipping RAD reader test";

    const fs::path outdir = ta.getFolder("gsplat_out");
    ddb::buildGsplat(ply.string(), outdir.string());
    const fs::path modelRad = outdir / GsplatRadFileName;
    ASSERT_TRUE(fs::exists(modelRad));
    EXPECT_TRUE(isRad(modelRad)) << "model.rad must start with the RAD0 magic";

    // Coarse-chunk decode: positions/colours/opacities are populated and self-consistent.
    const ddb::RadCoarseSplats splats = ddb::readRadCoarseSplats(modelRad, /*maxChunks=*/1);
    ASSERT_GT(splats.count, 0u);
    EXPECT_EQ(splats.positions.size(), splats.count * 3);
    EXPECT_EQ(splats.colors.size(), splats.count * 3);
    EXPECT_EQ(splats.opacities.size(), splats.count);
    // Colours are display RGB in [0,1] (NOT SH coefficients).
    for (const float c : splats.colors) {
        EXPECT_GE(c, -0.02f);
        EXPECT_LE(c, 1.02f);
    }

    // Exact bounds across all chunks are well-formed and in the RAD coordinate space.
    std::array<double, 3> bmin{}, bmax{};
    ASSERT_TRUE(ddb::computeRadBounds(modelRad, bmin, bmax));
    for (int k = 0; k < 3; ++k)
        EXPECT_LE(bmin[k], bmax[k]);

    // On-the-fly thumbnail: generateThumb dispatches a .rad to the splat rasteriser.
    const fs::path thumb = ta.getPath("thumb.webp");
    ddb::generateThumb(modelRad, 256, thumb, /*forceRecreate=*/true, nullptr, nullptr);
    ASSERT_TRUE(fs::exists(thumb));
    EXPECT_TRUE(isWebp(thumb)) << "the on-the-fly RAD thumbnail must be a WEBP image";
}

// build-lod is mandatory: when it cannot be located, buildGsplat throws a
// BuildDepMissingException (so the build is deferred and retried once the tool is installed)
// and produces no artifacts.
TEST(gsplat, buildLodMissingToolThrows) {
    TestArea ta(TEST_NAME);

    // RAII guard: sets DDB_BUILDLOD_PATH on construction and restores it (plus refreshes
    // the discovery cache) on destruction, even when the test body throws.
    struct EnvRestorer {
        std::string key, prev;
        EnvRestorer(const char* k, const char* v) : key(k) {
            const char* old = std::getenv(k);
            prev = old ? old : "";
#ifdef _WIN32
            _putenv_s(k, v);
#else
            setenv(k, v, 1);
#endif
            ddb::buildlod::findBuildLodBinary(/*forceRefresh=*/true);
        }
        ~EnvRestorer() {
#ifdef _WIN32
            _putenv_s(key.c_str(), prev.c_str());
#else
            setenv(key.c_str(), prev.c_str(), 1);
#endif
            ddb::buildlod::findBuildLodBinary(/*forceRefresh=*/true);
        }
    };

    // Force "no tool" deterministically via the authoritative env override.
    const std::string missing = (ta.getFolder("nope") / "build-lod-missing").string();
    EnvRestorer guard("DDB_BUILDLOD_PATH", missing.c_str());
    EXPECT_FALSE(ddb::buildlod::isBuildLodAvailable());

    const fs::path ply = ta.getPath("scene.ply");
    writeSplatPly(ply, 64, 0);
    const fs::path outdir = ta.getFolder("gsplat_out");

    EXPECT_THROW(ddb::buildGsplat(ply.string(), outdir.string()), BuildDepMissingException);
    EXPECT_FALSE(fs::exists(outdir / GsplatRadFileName)) << "no tool -> no model.rad";
    EXPECT_FALSE(fs::exists(outdir / GsplatFileName)) << "no tool -> no intermediate model.spz";
}

TEST(gsplat, ksplatRequiresExternalTool) {
    TestArea ta(TEST_NAME);
    const fs::path ksplat = ta.getPath("scene.ksplat");
    { std::ofstream o(ksplat.string(), std::ios::binary); o << "ksplatdata"; }

    const fs::path spz = ta.getPath("scene.spz");
    try {
        ddb::convertToSpz(ksplat.string(), spz.string());
        FAIL() << "Expected BuildDepMissingException";
    } catch (const BuildDepMissingException& e) {
        const auto& deps = e.getMissingDependencies();
        ASSERT_FALSE(deps.empty());
        EXPECT_EQ(deps[0], "splat-transform");
    }
    EXPECT_FALSE(fs::exists(spz));
}

TEST(gsplat, nonSplatPlyConversionThrows) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("cloud.ply");
    writePointCloudPly(ply, 10);
    const fs::path spz = ta.getPath("cloud.spz");
    EXPECT_THROW(ddb::convertToSpz(ply.string(), spz.string()), AppException);
}

// ---------------------------------------------------------------------------
// Georeferencing (ODX convention)
// ---------------------------------------------------------------------------

TEST(gsplat, parseCoordsAndWriteGeoref) {
    TestArea ta(TEST_NAME);
    const fs::path coords = ta.getPath("coords.txt");
    {
        std::ofstream o(coords.string());
        o << "WGS84 UTM 17N\n";
        o << "263892 4156842\n";
        o << "0 0 1.5\n"; // extra content ignored
    }

    GsplatGeoref g;
    ASSERT_TRUE(ddb::parseCoordsFile(coords, g));
    EXPECT_TRUE(g.valid);
    EXPECT_EQ(g.srs, "WGS84 UTM 17N");
    EXPECT_DOUBLE_EQ(g.offset[0], 263892.0);
    EXPECT_DOUBLE_EQ(g.offset[1], 4156842.0);

    const fs::path georef = ta.getPath("georef.json");
    ddb::writeGeoref(g, georef);
    ASSERT_TRUE(fs::exists(georef));

    std::ifstream in(georef.string());
    json j;
    in >> j;
    EXPECT_EQ(j["srs"].get<std::string>(), "WGS84 UTM 17N");
    ASSERT_TRUE(j["offset"].is_array());
    EXPECT_DOUBLE_EQ(j["offset"][0].get<double>(), 263892.0);
    EXPECT_DOUBLE_EQ(j["offset"][1].get<double>(), 4156842.0);
}

TEST(gsplat, parseCoordsRejectsMalformed) {
    TestArea ta(TEST_NAME);
    const fs::path coords = ta.getPath("bad.txt");
    { std::ofstream o(coords.string()); o << "WGS84 UTM 17N\n"; } // missing offset line

    GsplatGeoref g;
    EXPECT_FALSE(ddb::parseCoordsFile(coords, g));
    EXPECT_FALSE(g.valid);
}

TEST(gsplat, buildWithGerefWritesSidecar) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("scene.ply");
    writeSplatPly(ply, 40, 0);

    GsplatGeoref g;
    g.srs = "EPSG:32617";
    g.offset = {263892.0, 4156842.0, 0.0};
    g.valid = true;

    const fs::path outdir = ta.getFolder("gsplat_geo");
    ddb::buildGsplat(ply.string(), outdir.string(), g);

    // The delivery artifact is present (model.rad when build-lod ran, else model.spz).
    EXPECT_TRUE(fs::exists(outdir / GsplatRadFileName) || fs::exists(outdir / GsplatFileName));
    ASSERT_TRUE(fs::exists(outdir / GsplatGeorefFileName));

    std::ifstream in((outdir / GsplatGeorefFileName).string());
    json j;
    in >> j;
    EXPECT_EQ(j["srs"].get<std::string>(), "EPSG:32617");
}

// ---------------------------------------------------------------------------
// On-demand thumbnails (size honoured at request time)
// ---------------------------------------------------------------------------

TEST(gsplat, thumbnailFromSpzAtTwoSizes) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("scene.ply");
    writeSplatPly(ply, 400, 1);

    // Convert directly to a standalone .spz so the splat thumbnail renderer is exercised
    // independently of the build pipeline (which drops model.spz in favour of model.rad).
    const fs::path spz = ta.getPath("scene.spz");
    ddb::convertToSpz(ply.string(), spz.string());
    ASSERT_TRUE(fs::exists(spz));

    const fs::path thumb128 = ta.getPath("thumb_128.webp");
    const fs::path thumb512 = ta.getPath("thumb_512.webp");

    ddb::generateThumb(spz, 128, thumb128, true);
    ddb::generateThumb(spz, 512, thumb512, true);

    ASSERT_TRUE(fs::exists(thumb128));
    ASSERT_TRUE(fs::exists(thumb512));
    EXPECT_TRUE(isWebp(thumb128));
    EXPECT_TRUE(isWebp(thumb512));
    EXPECT_GT(fileSize(thumb128), 0u);
    // A larger target size yields more pixels -> larger output for the same scene.
    EXPECT_GT(fileSize(thumb512), fileSize(thumb128));
}

// ---------------------------------------------------------------------------
// Build-pipeline integration (DB -> isBuildable -> build -> isBuildComplete)
// ---------------------------------------------------------------------------

TEST(gsplat, buildPipelineIntegration) {
    // recreateIfExists = true: this test calls initIndex(), which refuses to run
    // against a pre-existing .ddb. Start from a clean area so the test is idempotent
    // across repeated runs.
    TestArea ta(TEST_NAME, true);
    const fs::path dsFolder = ta.getFolder("");
    const fs::path ply = ta.getPath("scene.ply");
    writeSplatPly(ply, 120, 1);

    // Gaussian Splat builds now require build-lod (model.rad is the sole delivery artifact).
    ddb::buildlod::findBuildLodBinary(/*forceRefresh=*/true);
    if (!ddb::buildlod::isBuildLodAvailable())
        GTEST_SKIP() << "build-lod not discoverable; Gaussian Splat builds require it";

    ddb::initIndex(dsFolder.string());
    auto db = ddb::open(dsFolder.string(), true);
    ddb::addToIndex(db.get(), {ply.string()});

    const auto rel = fs::relative(ply, dsFolder).string();

    Entry e;
    ASSERT_TRUE(ddb::getEntry(db.get(), rel, e));
    EXPECT_EQ(e.type, EntryType::GaussianSplat);

    std::string subfolder;
    EXPECT_TRUE(ddb::isBuildable(db.get(), rel, subfolder));
    EXPECT_EQ(subfolder, "gsplat");

    EXPECT_FALSE(ddb::isBuildComplete(db.get(), rel));
    ddb::build(db.get(), rel, "", false);
    EXPECT_TRUE(ddb::isBuildComplete(db.get(), rel));

    const fs::path gsplatDir = fs::path(db->buildDirectory()) / e.hash / "gsplat";
    // model.rad is the sole delivery artifact; no intermediate .spz, no build-time thumbnail.
    const fs::path modelRad = gsplatDir / GsplatRadFileName;
    ASSERT_TRUE(fs::exists(modelRad)) << "model.rad is the delivery artifact";
    EXPECT_GT(fileSize(modelRad), 0u);
    EXPECT_FALSE(fs::exists(gsplatDir / GsplatFileName)) << "no intermediate model.spz is kept";
    EXPECT_FALSE(fs::exists(gsplatDir / GsplatThumbFileName))
        << "thumbnails are rendered on demand from the .rad, not at build time";
}

} // namespace
