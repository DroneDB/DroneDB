/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <algorithm>
#include <cmath>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include <zlib.h>
#ifndef _WIN32
#include <unistd.h> // geteuid
#endif

#include "build.h"
#include "dbops.h"
#include "ddb.h"
#include "entry.h"
#include "fake_model_tools.h"
#include "exceptions.h"
#include "gsplat.h"
#include "buildlod_runner.h"
#include "gtest/gtest.h"
#include "load-spz.h"
#include "ply.h"
#include "rad.h"
#include "stac.h"
#include "splat_convert.h"
#include "splat_format.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"
#include "thumbs.h"

namespace {

using namespace ddb;

// Resolve the shared build-lod tool (always refreshing discovery so a pinned override from a
// sibling test cannot leave a stale cache), then skip when it is missing. When
// DDB_REQUIRE_BUILDLOD is set the skip becomes a hard failure: the tool must be shipped next
// to ddbtest and silently skipping build-dependent tests in that configuration is a defect.
#define REQUIRE_BUILDLOD_OR_SKIP(msg) \
    do { ddb::buildlod::findBuildLodBinary(/*forceRefresh=*/true); \
         if (!ddb::buildlod::isBuildLodAvailable()) { \
             if (std::getenv("DDB_REQUIRE_BUILDLOD") != nullptr) FAIL() << (msg) << " (DDB_REQUIRE_BUILDLOD is set; build-lod must be present next to ddbtest)"; \
             GTEST_SKIP() << (msg); } } while (0)

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

// Writes an SPZ of the requested format version using the vendored spz encoder.
// PackOptions.version accepts 1-3 (legacy gzip container) and 4 ("NGSP" ZSTD per-stream
// container); saveSpz rejects nothing inside that range, so v2 is used for the legacy fixture.
bool writeSpzVersion(const fs::path& ply, const fs::path& spz, uint32_t version) {
    spz::GaussianCloud cloud = spz::loadSplatFromPly(ply.string(), spz::UnpackOptions{});
    if (cloud.numPoints <= 0)
        return false;
    spz::PackOptions opts;
    opts.version = version;
    return spz::saveSpz(cloud, opts, spz.string());
}

// First 4 bytes of a file as an ASCII magic tag (empty when shorter).
std::string magic4(const fs::path& p) {
    std::ifstream in(p.string(), std::ios::binary);
    char m[4] = {0};
    in.read(m, 4);
    return in.gcount() == 4 ? std::string(m, 4) : std::string();
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

std::string readText(const fs::path& p) {
    std::ifstream in(p.string(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
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

// Names in a folder, sorted.
std::vector<std::string> namesIn(const fs::path& dir) {
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(dir))
        names.push_back(e.path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

// A splat PLY whose x of the first splat is NaN: build-lod refuses it after decoding it.
void writeNanSplatPly(const fs::path& path) {
    writeSplatPly(path, 40, 0);
    std::fstream f(path.string(), std::ios::in | std::ios::out | std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const auto body = content.find("end_header\n") + std::string("end_header\n").size();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    f.seekp(static_cast<std::streamoff>(body));
    f.write(reinterpret_cast<const char*>(&nan), sizeof(nan));
}

// The same splats as writeSplatPly(path, n, 0), in the ASCII PLY format that build-lod does not read.
void writeAsciiSplatPly(const fs::path& path, int n) {
    std::ofstream out(path.string(), std::ios::binary);
    out << "ply\nformat ascii 1.0\nelement vertex " << n << "\n";
    for (const char* p : {"x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2", "opacity", "scale_0", "scale_1", "scale_2",
                          "rot_0", "rot_1", "rot_2", "rot_3"})
        out << "property float " << p << "\n";
    out << "end_header\n";
    for (int i = 0; i < n; ++i)
        out << i % 10 << " " << i / 10 << " 0 0.2 0.1 -0.1 5 -3 -3 -3 1 0 0 0\n";
}

// The text of the AppException that buildGsplat throws ("" when it does not throw).
std::string buildFailure(const fs::path& input, const fs::path& outdir) {
    try {
        ddb::buildGsplat(input.string(), outdir.string());
    } catch (const AppException& e) {
        return e.what();
    }
    return std::string();
}

// The header lines (after the "ply" line, up to the end of the vertex element's properties) of a
// splat PLY that build-lod reads; the tests change one thing of it at a time.
std::vector<std::string> splatHeaderLines(const std::string& format = "binary_little_endian 1.0", int fRest = 0,
                                          const std::string& without = std::string()) {
    std::vector<std::string> lines = {"format " + format, "element vertex 4"};
    std::vector<std::string> props = {"x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2"};
    for (int i = 0; i < fRest; ++i)
        props.push_back("f_rest_" + std::to_string(i));
    for (const char* p : {"opacity", "scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2", "rot_3"})
        props.push_back(p);
    for (const auto& p : props)
        if (p != without)
            lines.push_back("property float " + p);
    return lines;
}

// A PLY of the given header lines and an arbitrary body: for what only reads the header.
void writeHeaderOnlyPly(const fs::path& path, const std::vector<std::string>& lines) {
    std::ofstream out(path.string(), std::ios::binary);
    out << "ply\n";
    for (const auto& l : lines)
        out << l << "\n";
    out << "end_header\n";
    out << std::string(256, '\0');
}

// A SuperSplat compressed PLY: one chunk record per 256 splats, each splat four packed uints.
void writeCompressedSplatPly(const fs::path& path, int n) {
    const int chunks = (n + 255) / 256;
    std::ofstream out(path.string(), std::ios::binary);
    out << "ply\nformat binary_little_endian 1.0\nelement chunk " << chunks << "\n";
    for (const char* p : {"min_x", "min_y", "min_z", "max_x", "max_y", "max_z", "min_scale_x", "min_scale_y",
                          "min_scale_z", "max_scale_x", "max_scale_y", "max_scale_z", "min_r", "min_g", "min_b",
                          "max_r", "max_g", "max_b"})
        out << "property float " << p << "\n";
    out << "element vertex " << n << "\n";
    for (const char* p : {"packed_position", "packed_rotation", "packed_scale", "packed_color"})
        out << "property uint " << p << "\n";
    out << "end_header\n";
    const float chunk[18] = {0, 0, 0, 10, 10, 1, -5, -5, -5, -2, -2, -2, 0, 0, 0, 1, 1, 1};
    for (int c = 0; c < chunks; ++c)
        out.write(reinterpret_cast<const char*>(chunk), sizeof(chunk));
    for (int i = 0; i < n; ++i) {
        const uint32_t position = (static_cast<uint32_t>(i * 7 % 2048) << 21) | (static_cast<uint32_t>(i % 1024) << 11) |
                                  static_cast<uint32_t>(i * 3 % 2048);
        const uint32_t rotation = (3u << 30) | (512u << 20) | (512u << 10) | 512u;
        const uint32_t scale = (1024u << 21) | (512u << 11) | 1024u;
        const uint32_t color = (128u << 24) | (128u << 16) | (128u << 8) | 255u;
        const uint32_t row[4] = {position, rotation, scale, color};
        out.write(reinterpret_cast<const char*>(row), sizeof(row));
    }
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

// A PLY that looks like a splat stays a GaussianSplat when build-lod cannot read it (the type is not
// downgraded to a point cloud); the file says what to change, so the build can tell the user.
TEST(gsplat, aSplatPlyBuildLodCannotReadStaysASplatAndNamesTheProblem) {
    TestArea ta(TEST_NAME);

    struct Case {
        std::string name;
        std::vector<std::string> header;
        std::string problem;
    };
    std::vector<Case> cases = {
        {"ascii", splatHeaderLines("ascii 1.0"), "ASCII splat PLY is not supported: export it as binary little-endian"},
        {"big_endian", splatHeaderLines("binary_big_endian 1.0"),
         "Big-endian splat PLY is not supported: export it as binary little-endian"},
        {"version", splatHeaderLines("binary_little_endian 2.0"), "PLY version '2.0' is not supported"},
        {"f_rest_12", splatHeaderLines("binary_little_endian 1.0", 12), "f_rest count 12 is not 0, 9, 24 or 45"},
        {"f_rest_50", splatHeaderLines("binary_little_endian 1.0", 50), "f_rest count 50 is not 0, 9, 24 or 45"},
        {"no_opacity", splatHeaderLines("binary_little_endian 1.0", 0, "opacity"), "missing properties: opacity"},
        {"no_rot_3", splatHeaderLines("binary_little_endian 1.0", 0, "rot_3"), "missing properties: rot_3"},
        {"no_f_dc_1", splatHeaderLines("binary_little_endian 1.0", 0, "f_dc_1"), "missing properties: f_dc_1"},
    };
    auto withExtra = [](std::vector<std::string> lines, const std::string& line) {
        lines.push_back(line);
        return lines;
    };
    cases.push_back({"list", withExtra(splatHeaderLines(), "property list uchar int extra"),
                     "the list property 'extra' in element 'vertex'"});
    auto sized = splatHeaderLines();
    sized.back() = "property float32 rot_3";
    cases.push_back({"sized_type_name", sized, "Property 'rot_3' has the type 'float32'"});

    for (const auto& c : cases) {
        const fs::path ply = ta.getPath(c.name + ".ply");
        writeHeaderOnlyPly(ply, c.header);

        EXPECT_EQ(ddb::fingerprint(ply), EntryType::GaussianSplat) << c.name;
        PlyInfo info;
        ASSERT_TRUE(ddb::getPlyInfo(ply, info)) << c.name;
        EXPECT_TRUE(info.isSplat) << c.name;
        EXPECT_NE(info.splatProblem.find(c.problem), std::string::npos) << c.name << ": " << info.splatProblem;
        EXPECT_FALSE(ddb::isBuildableSplatPly(ddb::readPlyHeader(ply))) << c.name;
    }
}

// build-lod counts the f_rest_* of the vertex element, and a count that is none of the standard ones
// is not a degree: it was clamped to the closest lower one, which hid the problem.
TEST(gsplat, theShDegreeComesFromTheVertexElementAndIsNotClamped) {
    TestArea ta(TEST_NAME);

    auto lines = splatHeaderLines();
    lines.push_back("element extra 4");
    for (int i = 0; i < 9; ++i)
        lines.push_back("property float f_rest_" + std::to_string(i));
    const fs::path other = ta.getPath("other_element.ply");
    writeHeaderOnlyPly(other, lines);
    PlyInfo info;
    ASSERT_TRUE(ddb::getPlyInfo(other, info));
    EXPECT_TRUE(info.isSplat);
    EXPECT_EQ(info.shDegree, 0) << "the f_rest_* of another element are not spherical harmonics";
    EXPECT_TRUE(info.splatProblem.empty()) << info.splatProblem;

    const fs::path odd = ta.getPath("odd.ply");
    writeHeaderOnlyPly(odd, splatHeaderLines("binary_little_endian 1.0", 30));
    ASSERT_TRUE(ddb::getPlyInfo(odd, info));
    EXPECT_TRUE(info.isSplat);
    EXPECT_EQ(info.shDegree, -1) << "30 f_rest_* are no degree";

    // build-lod stops counting at the first f_rest_N that is missing
    auto gap = splatHeaderLines("binary_little_endian 1.0", 9);
    gap.erase(std::find(gap.begin(), gap.end(), "property float f_rest_4"));
    const fs::path gapped = ta.getPath("gap.ply");
    writeHeaderOnlyPly(gapped, gap);
    ASSERT_TRUE(ddb::getPlyInfo(gapped, info));
    EXPECT_NE(info.splatProblem.find("f_rest count 4 is not 0, 9, 24 or 45"), std::string::npos) << info.splatProblem;
}

// The SuperSplat compressed layout (a chunk element and packed vertices) is a splat that build-lod reads.
TEST(gsplat, aCompressedSplatPlyIsASplatBuildLodReads) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("compressed.ply");
    writeCompressedSplatPly(ply, 300);

    EXPECT_EQ(ddb::fingerprint(ply), EntryType::GaussianSplat);
    PlyInfo info;
    ASSERT_TRUE(ddb::getPlyInfo(ply, info));
    EXPECT_TRUE(info.isSplat);
    EXPECT_EQ(info.shDegree, 0);
    EXPECT_EQ(info.vertexCount, 300u);
    EXPECT_TRUE(info.splatProblem.empty()) << info.splatProblem;
    EXPECT_TRUE(ddb::isBuildableSplatPly(ddb::readPlyHeader(ply)));

    // What build-lod needs of it, rule by rule
    auto header = [&](const std::string& drop) {
        PlyHeader h = ddb::readPlyHeader(ply);
        for (auto& e : h.elements)
            e.properties.erase(std::remove_if(e.properties.begin(), e.properties.end(),
                                              [&](const PlyProperty& p) { return p.name == drop; }),
                               e.properties.end());
        return ddb::plySplatBuildProblem(h);
    };
    EXPECT_NE(header("packed_color").find("missing properties: packed_color"), std::string::npos);
    EXPECT_NE(header("max_scale_z").find("missing chunk properties: max_scale_z"), std::string::npos);

    PlyHeader fewChunks = ddb::readPlyHeader(ply);
    fewChunks.elements[fewChunks.findElement("chunk")].count = 1;
    EXPECT_NE(ddb::plySplatBuildProblem(fewChunks).find("1 chunk records for 300 splats"), std::string::npos);

    PlyHeader badSh = ddb::readPlyHeader(ply);
    PlyElement sh;
    sh.name = "sh";
    sh.count = 300;
    for (int i = 0; i < 12; ++i) {
        PlyProperty p;
        p.name = "f_rest_" + std::to_string(i);
        p.typeName = "uchar";
        sh.properties.push_back(p);
    }
    badSh.elements.push_back(sh);
    EXPECT_NE(ddb::plySplatBuildProblem(badSh).find("f_rest count 12 is not 0, 9, 24 or 45"), std::string::npos);
}

TEST(gsplat, looksLikeHelpers) {
    TestArea ta(TEST_NAME);

    const fs::path spz = ta.getPath("ok.spz");
    {
        std::ofstream o(spz.string(), std::ios::binary);
        const char magic[4] = {'N', 'G', 'S', 'P'};
        o.write(magic, sizeof(magic));
    }
    EXPECT_TRUE(ddb::looksLikeSplat(spz));

    const fs::path notSpz = ta.getPath("bad.spz");
    { std::ofstream o(notSpz.string(), std::ios::binary); o << "junk"; }
    EXPECT_FALSE(ddb::looksLikeSplat(notSpz));

    const fs::path splat = ta.getPath("ok.splat");
    writeSplatBinary(splat, 3);
    EXPECT_TRUE(ddb::looksLikeSplat(splat));

    const fs::path badSplat = ta.getPath("bad.splat");
    { std::ofstream o(badSplat.string(), std::ios::binary); o << "12345"; } // 5 bytes
    EXPECT_FALSE(ddb::looksLikeSplat(badSplat));
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
    EXPECT_TRUE(ddb::looksLikeSplat(spz));
    // The delivery artifact must be gzip-based SPZ (v3) for the Spark viewer.
    EXPECT_TRUE(isGzip(spz)) << "convertToSpz must emit gzip-based SPZ v3, not NGSP/ZSTD v4";

    GaussianSplatInfo info;
    ASSERT_TRUE(ddb::getGaussianSplatInfo(spz.string(), info));
    EXPECT_EQ(info.splatCount, 100u);
    EXPECT_EQ(info.shDegree, 1);
}

// A conversion replaces its output in one step, and never replaces its input
TEST(gsplat, convertToSpzWritesItsOutputAtomicallyAndNeverOverItsInput) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("scene.ply");
    writeSplatPly(ply, 20, 1);
    const fs::path spz = ta.getPath("scene.spz");
    ddb::convertToSpz(ply.string(), spz.string());
    const std::string before = readText(spz);
    ASSERT_FALSE(before.empty());

    // The input is not the output, however the path is written
    for (const fs::path& same : {spz, ta.getPath("sub") / ".." / "scene.spz"}) {
        try {
            ddb::convertToSpz(spz.string(), same.string());
            ADD_FAILURE() << "converted " << same << " onto itself";
        } catch (const InvalidArgsException& e) {
            EXPECT_NE(std::string(e.what()).find("choose a different output path"), std::string::npos) << e.what();
        }
        EXPECT_EQ(readText(spz), before) << "the input was written to";
    }

    // An output that is replaced is replaced whole; one that cannot be is left as it was, with no
    // part of the new file beside it (here the output path is a folder)
    const fs::path other = ta.getPath("other.spz");
    { std::ofstream(other.string(), std::ios::binary) << "the previous output"; }
    ddb::convertToSpz(ply.string(), other.string());
    EXPECT_EQ(readText(other), before);

    const fs::path folder = ta.getFolder("out.spz");
    EXPECT_THROW(ddb::convertToSpz(ply.string(), folder.string()), AppException);
    EXPECT_TRUE(fs::is_directory(folder));
    EXPECT_THROW(ddb::convertToSpz(spz.string(), folder.string()), AppException);
    EXPECT_EQ(namesIn(ta.getFolder("")), (std::vector<std::string>{"other.spz", "out.spz", "scene.ply", "scene.spz"}))
        << "no temporary file is left";
    EXPECT_TRUE(namesIn(folder).empty());
}

TEST(gsplat, convertSplatBinaryToSpz) {
    TestArea ta(TEST_NAME);
    const fs::path splat = ta.getPath("scene.splat");
    writeSplatBinary(splat, 64);

    const fs::path spz = ta.getPath("scene.spz");
    ddb::convertToSpz(splat.string(), spz.string());

    ASSERT_TRUE(fs::exists(spz));
    EXPECT_TRUE(ddb::looksLikeSplat(spz));

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

    const fs::path outdir = ta.getFolder("gsplat_out");

    ddb::buildlod::findBuildLodBinary(/*forceRefresh=*/true);
    if (!ddb::buildlod::isBuildLodAvailable()) {
        // build-lod is a mandatory tool: without it the build fails (it is not deferred)
        // and no artifacts are produced.
        EXPECT_THROW(ddb::buildGsplat(ply.string(), outdir.string()), AppException);
        REQUIRE_BUILDLOD_OR_SKIP("build-lod not discoverable; skipping artifact assertions");
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

    // Which way is up, estimated from the splats: a unit vector, how sure it is, and how it was found
    ASSERT_TRUE(b.contains("up") && b.contains("upConfidence") && b.contains("upSource"));
    ASSERT_EQ(b["up"].size(), 3u);
    const double length = std::sqrt(std::pow(b["up"][0].get<double>(), 2) + std::pow(b["up"][1].get<double>(), 2) +
                                    std::pow(b["up"][2].get<double>(), 2));
    EXPECT_NEAR(length, 1.0, 1e-5);
    EXPECT_GE(b["upConfidence"].get<double>(), 0.0);
    EXPECT_LE(b["upConfidence"].get<double>(), 1.0);
    const std::string source = b["upSource"].get<std::string>();
    EXPECT_TRUE(source == "ground-plane" || source == "pca") << source;
}

// build-lod exits 0 after it refuses an input and only prints why: the error of the build says it
TEST(gsplat, buildFailureNamesTheReasonBuildLodPrinted) {
    TestArea ta(TEST_NAME);
    REQUIRE_BUILDLOD_OR_SKIP("build-lod not discoverable; skipping failure-reason assertions");

    const fs::path nan = ta.getPath("nan.ply");
    writeNanSplatPly(nan);
    const std::string nanFailure = buildFailure(nan, ta.getFolder("out_nan"));
    EXPECT_NE(nanFailure.find("build-lod failed for"), std::string::npos) << nanFailure;
    EXPECT_NE(nanFailure.find("invalid splats"), std::string::npos) << nanFailure;

    EXPECT_FALSE(fs::exists(ta.getPath("out_nan/model.rad")));
    EXPECT_TRUE(namesIn(ta.getPath("out_nan")).empty()) << "a failed build leaves nothing in the output folder";
    EXPECT_EQ(namesIn(ta.getFolder("")).size(), 2u) << "and nothing beside the input";
}

// build-lod writes "<name>-lod.rad" beside its input and skips an input named "*-lod.spz": the
// build stages the input in the output folder instead, so the dataset folder is never written to,
// a user's own file there survives, a dataset that cannot be written to builds, and the folder that
// is published holds the delivery files only.
TEST(gsplat, buildLeavesTheDatasetFolderAloneAndPublishesOnlyItsOutputs) {
    TestArea ta(TEST_NAME);
    REQUIRE_BUILDLOD_OR_SKIP("build-lod not discoverable; skipping staging assertions");

    const fs::path dataset = ta.getFolder("dataset");
    const fs::path ply = dataset / "scene.ply";
    writeSplatPly(ply, 80, 1);
    const fs::path other = dataset / "other.spz";
    ASSERT_TRUE(writeSpzVersion(ply, other, 3));
    const fs::path lodNamed = dataset / "scene-lod.spz";
    ASSERT_TRUE(writeSpzVersion(ply, lodNamed, 3));
    const fs::path userRad = dataset / "scene-lod.rad";
    {
        std::ofstream(userRad.string(), std::ios::binary) << "the user's own file";
    }
    const auto before = namesIn(dataset);

    for (const fs::path& input : {ply, lodNamed}) {
        const fs::path outdir = ta.getFolder("out_" + input.stem().string());
        ddb::buildGsplat(input.string(), outdir.string());
        EXPECT_EQ(namesIn(outdir), (std::vector<std::string>{GsplatBoundsFileName, GsplatRadFileName}))
            << input.filename();
        EXPECT_TRUE(isRad(outdir / GsplatRadFileName)) << input.filename();
    }

    EXPECT_EQ(namesIn(dataset), before) << "no file added or removed in the dataset folder";
    std::ifstream in(userRad.string(), std::ios::binary);
    EXPECT_EQ(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), "the user's own file");

#ifndef _WIN32
    if (geteuid() != 0) {
        fs::permissions(dataset, fs::perms::owner_read | fs::perms::owner_exec);
        const fs::path outdir = ta.getFolder("out_readonly");
        std::string failure;
        try {
            ddb::buildGsplat(ply.string(), outdir.string());
        } catch (const AppException& e) {
            failure = e.what();
        }
        fs::permissions(dataset, fs::perms::owner_all);
        EXPECT_EQ(failure, "") << "a read-only dataset folder builds";
        EXPECT_TRUE(isRad(outdir / GsplatRadFileName));
    }
#endif
}

// A v4 (NGSP) SPZ cannot be read by build-lod: the build transcodes it to v3 first, in a scratch
// folder that is not published
TEST(gsplat, buildAcceptsAV4Spz) {
    TestArea ta(TEST_NAME);
    REQUIRE_BUILDLOD_OR_SKIP("build-lod not discoverable; skipping v4 build assertions");

    const fs::path ply = ta.getPath("scene.ply");
    writeSplatPly(ply, 64, 1);
    const fs::path v4 = ta.getPath("scene_v4.spz");
    ASSERT_TRUE(writeSpzVersion(ply, v4, 4));
    ASSERT_EQ(magic4(v4), "NGSP");
    const auto sourceBefore = fileSize(v4);

    const fs::path outdir = ta.getFolder("out");
    ddb::buildGsplat(v4.string(), outdir.string());

    EXPECT_EQ(namesIn(outdir), (std::vector<std::string>{GsplatBoundsFileName, GsplatRadFileName}));
    ASSERT_TRUE(isRad(outdir / GsplatRadFileName));
    EXPECT_EQ(fileSize(v4), sourceBefore);
    EXPECT_EQ(magic4(v4), "NGSP") << "the upload itself is not rewritten";

    // the splats made it: the bounds are those of the 10 x 7 grid of the fixture
    std::ifstream bin((outdir / GsplatBoundsFileName).string(), std::ios::binary);
    const json b = json::parse(bin);
    EXPECT_NEAR(b["max"][0].get<double>(), 9.0, 0.1);
    EXPECT_NEAR(b["max"][1].get<double>(), 6.0, 0.1);
}

// The RAD reader decodes the coarse first chunk for on-the-fly previews, and generateThumb
// dispatches a .rad to the splat rasteriser - the same on-demand model used for COPC point
// clouds. No build-time thumbnail or intermediate .spz is produced.
TEST(gsplat, radReaderAndOnTheFlyThumbnail) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("scene.ply");
    writeSplatPly(ply, 4000, 3); // enough splats for a non-trivial LOD tree, full SH

    REQUIRE_BUILDLOD_OR_SKIP("build-lod not discoverable; skipping RAD reader test");

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

    // So is the shape of each splat: a scale along each of its axes and a unit rotation
    EXPECT_EQ(splats.scales.size(), splats.count * 3);
    EXPECT_EQ(splats.rotations.size(), splats.count * 4);
    for (const float scale : splats.scales) {
        EXPECT_TRUE(std::isfinite(scale));
        EXPECT_GE(scale, 0.0f);
    }
    for (size_t i = 0; i < splats.count; ++i) {
        const float* q = &splats.rotations[i * 4];
        EXPECT_NEAR(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3], 1.0f, 1e-3f) << "splat " << i;
    }

    // Exact bounds across all chunks are well-formed and in the RAD coordinate space.
    std::array<double, 3> bmin{}, bmax{};
    ASSERT_TRUE(ddb::computeRadBounds(modelRad, bmin, bmax));
    for (int k = 0; k < 3; ++k)
        EXPECT_LE(bmin[k], bmax[k]);

    // The finest chunks keep the splats of the input: scale exp(-3) on every axis (the fixture's
    // log scale), turned by the identity. A scale or a rotation read wrongly cannot pass this.
    const ddb::RadCoarseSplats all = ddb::readRadCoarseSplats(modelRad, /*maxChunks=*/0);
    size_t asWritten = 0;
    for (size_t i = 0; i < all.count; ++i) {
        const float* s = &all.scales[i * 3];
        const float* q = &all.rotations[i * 4];
        const float expected = std::exp(-3.0f);
        if (std::abs(s[0] - expected) < 0.1f * expected && std::abs(s[1] - expected) < 0.1f * expected &&
            std::abs(s[2] - expected) < 0.1f * expected && std::abs(q[3]) > 0.99f)
            ++asWritten;
    }
    EXPECT_GT(asWritten, 0u) << "no splat of the file has the scale and rotation the input gave";

    // On-the-fly thumbnail: generateThumb dispatches a .rad to the splat rasteriser.
    const fs::path thumb = ta.getPath("thumb.webp");
    ddb::generateThumb(modelRad, 256, thumb, /*forceRecreate=*/true, nullptr, nullptr);
    ASSERT_TRUE(fs::exists(thumb));
    EXPECT_TRUE(isWebp(thumb)) << "the on-the-fly RAD thumbnail must be a WEBP image";
}

// The .rad must keep the input coordinate space: build-lod encodes centres as exact f32
// (vendor/spark/rust/spark-lib/src/rad.rs resolve_center_encoding -> F32LeBytes) and never
// re-centres or rescales the scene, so the bounds streamed from every chunk must sit inside
// the input extents and still span almost the whole x/y range. An axis swap (the fixture
// spans 9 in x vs 399 in y) or a scaling regression cannot pass both coverage checks.
TEST(gsplat, radBoundsMatchInput) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("scene.ply");
    writeSplatPly(ply, 4000, 3); // same fixture as radReaderAndOnTheFlyThumbnail

    REQUIRE_BUILDLOD_OR_SKIP("build-lod not discoverable; skipping RAD bounds test");

    const fs::path outdir = ta.getFolder("gsplat_out");
    ddb::buildGsplat(ply.string(), outdir.string());
    const fs::path modelRad = outdir / GsplatRadFileName;
    ASSERT_TRUE(fs::exists(modelRad));
    EXPECT_TRUE(isRad(modelRad));

    // Input extents from the writeSplatPly layout: x = i % 10, y = i / 10, z = (i % 3) * 0.1.
    const double minIn[3] = {0.0, 0.0, 0.0};
    const double maxIn[3] = {9.0, 399.0, 0.2};

    std::array<double, 3> bmin{}, bmax{};
    ASSERT_TRUE(ddb::computeRadBounds(modelRad, bmin, bmax));
    for (int k = 0; k < 3; ++k) {
        // Fine chunks store exact f32 centres (rad.rs resolve_center_encoding -> f32_lebytes);
        // coarse LOD chunks may use range encodings (r8/r8_delta), so allow a slack of ~half a
        // byte-step (span/254) per axis - tight enough that an axis swap or scaling fails.
        const double pad = std::max(0.05, (maxIn[k] - minIn[k]) * (0.5 / 254.0));
        EXPECT_LE(bmin[k], bmax[k]) << "axis " << k;
        EXPECT_GE(bmin[k], minIn[k] - pad) << "axis " << k << " min escaped the input extents";
        EXPECT_LE(bmax[k], maxIn[k] + pad) << "axis " << k << " max escaped the input extents";
    }

    // The fine chunks retain every input splat, so the decoded extents must cover at least
    // 95% of the input x and y spreads (9.0 and 399.0).
    EXPECT_GE(bmax[0] - bmin[0], 0.95 * (maxIn[0] - minIn[0])) << "x coverage regressed";
    EXPECT_GE(bmax[1] - bmin[1], 0.95 * (maxIn[1] - minIn[1])) << "y coverage regressed";
}

// Characterisation test for the full-decode path of readRadCoarseSplats (maxChunks = INT_MAX).
// RAD is coarse-to-fine: every input splat appears at least once across the chunks and the
// LOD levels may duplicate splats, so the decoded total is bounded below by the input count
// and (as a runaway guard) above by 4x. The exact observed total is pinned by
// kExpectedFullDecodeCount below - see the recording note there.
TEST(gsplat, radFullDecodeCharacterization) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("scene.ply");
    writeSplatPly(ply, 4000, 3); // enough splats for a non-trivial LOD tree, full SH

    REQUIRE_BUILDLOD_OR_SKIP("build-lod not discoverable; skipping RAD full-decode test");

    const fs::path outdir = ta.getFolder("gsplat_out");
    ddb::buildGsplat(ply.string(), outdir.string());
    const fs::path modelRad = outdir / GsplatRadFileName;
    ASSERT_TRUE(fs::exists(modelRad));

    constexpr size_t kInputSplats = 4000;

    const ddb::RadCoarseSplats full = ddb::readRadCoarseSplats(modelRad, INT_MAX);
    EXPECT_GE(full.count, kInputSplats) << "full decode must yield at least one entry per input splat";
    EXPECT_LE(full.count, kInputSplats * 4u) << "runaway duplication across LOD chunks";
    EXPECT_EQ(full.positions.size(), full.count * 3);
    EXPECT_EQ(full.colors.size(), full.count * 3);
    EXPECT_EQ(full.opacities.size(), full.count);

    const ddb::RadCoarseSplats coarse = ddb::readRadCoarseSplats(modelRad, /*maxChunks=*/1);
    ASSERT_GT(coarse.count, 0u);
    EXPECT_EQ(coarse.positions.size(), coarse.count * 3);
    EXPECT_EQ(coarse.colors.size(), coarse.count * 3);
    EXPECT_EQ(coarse.opacities.size(), coarse.count);
    for (const float c : coarse.colors) {
        EXPECT_GE(c, -0.02f);
        EXPECT_LE(c, 1.02f);
    }

    // The coarsest chunk is always a subset of the full decode (chunk 0 is read by both).
    EXPECT_LE(coarse.count, full.count);

    // Characterisation pin for the exact decoded total: stamp it with the recording date and
    // the spark revision in use (submodule pinned at 750812d, quality build-lod profile; the
    // rad.rs byte layout is itself pinned by tests/data/spark-rad-format.sha256), and expect
    // reviewers to accept a re-record only for an intentional LOD change - build-lod method,
    // detail ratio, or a vendor/spark bump.
    // TODO(record-build-lod): NOT RECORDED - the authoring machine has no build-lod binary and
    // no cargo on PATH (scripts/build-buildlod.ps1 needs cargo and does not install it), so the
    // observed coarse/full counts could not be captured. 0 disables the pin assertion. Rerun
    //   build\ddbtest.exe --gtest_filter=gsplat.radFullDecodeCharacterization
    // with build-lod on PATH, read the observed value printed below, set the constant to it,
    // prefix this block with "pin recorded <date> at spark <sha>", and - if the log shows
    // coarse < full - tighten the comparison above to EXPECT_LT (if coarse == full for this
    // size, keep EXPECT_LE and note the observation here).
    const size_t kExpectedFullDecodeCount = 0;
    if (kExpectedFullDecodeCount == 0) {
        std::cout << "[radFullDecodeCharacterization] observed full=" << full.count
                  << " coarse=" << coarse.count << " -> set kExpectedFullDecodeCount"
                  << std::endl;
        RecordProperty("FullDecodeCount", static_cast<int>(full.count));
        RecordProperty("CoarseCount", static_cast<int>(coarse.count));
    } else {
        const size_t tol = kExpectedFullDecodeCount / 50u; // +/- 2%
        EXPECT_GE(full.count, kExpectedFullDecodeCount - tol) << "full decode dropped entries";
        EXPECT_LE(full.count, kExpectedFullDecodeCount + tol) << "full decode grew";
    }
}

// Malformed .rad handling. Locks the CURRENT behaviour of the reader (src/library/rad.cpp):
//  - bad bytes: readHeader() throws AppException("RAD: bad magic ...") as soon as the first
//    u32 differs from 'RAD0', and computeRadBounds calls readHeader() before accumulating any
//    centre (src/library/rad.cpp, computeRadBounds -> readHeader), so it propagates the throw
//    instead of returning false.
//  - truncated payload: decodeChunk() reads the chunk's declared `bytes` from the file and
//    throws AppException("RAD: cannot read chunk payload") when the stream ends early, so
//    computeRadBounds over a half-length copy must throw (it reads every chunk).
TEST(gsplat, malformedRadThrows) {
    TestArea ta(TEST_NAME);

    // (a) Not a RAD at all - pure reader path, no build-lod needed.
    const fs::path bogus = ta.getPath("not_a_rad.rad");
    { std::ofstream o(bogus.string(), std::ios::binary); o << "NOTARADFILE"; }
    ASSERT_FALSE(isRad(bogus));

    EXPECT_THROW(ddb::readRadCoarseSplats(bogus, /*maxChunks=*/1), AppException);
    std::array<double, 3> bmin{}, bmax{};
    EXPECT_THROW(ddb::computeRadBounds(bogus, bmin, bmax), AppException);

    // (b) Real artifact chopped to half its length: the surviving header still lists every
    // chunk. NOTE: code-derived, not executed on the authoring machine (build-lod absent);
    // if computeRadBounds ever *succeeds* on the half file that is a reader gap (a truncated
    // delivery file accepted) - fix decodeChunk()/readHeader(), do not relax this assertion.
    const fs::path ply = ta.getPath("scene.ply");
    writeSplatPly(ply, 4000, 3);

    REQUIRE_BUILDLOD_OR_SKIP("build-lod not discoverable; skipping truncated-RAD assertions");

    const fs::path outdir = ta.getFolder("gsplat_out");
    ddb::buildGsplat(ply.string(), outdir.string());
    const fs::path modelRad = outdir / GsplatRadFileName;
    ASSERT_TRUE(fs::exists(modelRad));
    ASSERT_GT(fileSize(modelRad), 16u);

    const fs::path half = ta.getPath("half.rad");
    {
        std::ifstream in(modelRad.string(), std::ios::binary | std::ios::ate);
        ASSERT_TRUE(in.good());
        const auto halfSize = static_cast<size_t>(in.tellg()) / 2;
        ASSERT_GT(halfSize, 8u) << "the prefix must keep the magic + meta-length header";
        std::vector<uint8_t> buf(halfSize);
        in.seekg(0);
        in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(halfSize));
        std::ofstream out(half.string(), std::ios::binary);
        out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    }
    EXPECT_TRUE(isRad(half)) << "the surviving prefix keeps the RAD0 magic";

    // The header of the half file still lists every chunk, and a chunk that lies beyond the end
    // of the file is refused when the header is read (src/library/rad.cpp, readHeader).
    std::array<double, 3> hmin{}, hmax{};
    EXPECT_THROW(ddb::computeRadBounds(half, hmin, hmax), AppException);
}

// RAII guard: sets an environment variable on construction and restores it (plus refreshes
// the build-lod discovery cache) on destruction, even when the test body throws.
struct BuildLodEnvRestorer {
    EnvVarGuard env;
    BuildLodEnvRestorer(const char* k, const char* v) : env(k, v) {
        ddb::buildlod::findBuildLodBinary(/*forceRefresh=*/true);
    }
    ~BuildLodEnvRestorer() {
        env.restore();  // before the refresh, which reads it
        ddb::buildlod::findBuildLodBinary(/*forceRefresh=*/true);
    }
};

// Asserts that `fn` throws a plain AppException (not BuildDepMissingException) whose
// message mentions `tool`: a missing external tool fails the build instead of deferring it.
template <typename Fn>
void expectMissingToolFailure(Fn fn, const std::string& tool) {
    try {
        fn();
        FAIL() << "Expected AppException naming " << tool;
    } catch (const BuildDepMissingException& e) {
        FAIL() << "A missing tool must not defer the build: " << e.what();
    } catch (const AppException& e) {
        EXPECT_NE(std::string(e.what()).find(tool), std::string::npos) << e.what();
    }
}

// build-lod is mandatory: when it cannot be located, buildGsplat throws a plain
// AppException (the build fails; a missing tool is not a missing dependency, so it is
// never deferred) and produces no artifacts.
TEST(gsplat, buildLodMissingToolThrows) {
    TestArea ta(TEST_NAME);

    // Force "no tool" deterministically via the authoritative env override.
    const std::string missing = (ta.getFolder("nope") / "build-lod-missing").string();
    BuildLodEnvRestorer guard("DDB_BUILDLOD_PATH", missing.c_str());
    EXPECT_FALSE(ddb::buildlod::isBuildLodAvailable());

    const fs::path ply = ta.getPath("scene.ply");
    writeSplatPly(ply, 64, 0);
    const fs::path outdir = ta.getFolder("gsplat_out");

    expectMissingToolFailure([&] { ddb::buildGsplat(ply.string(), outdir.string()); }, "build-lod");
    EXPECT_FALSE(fs::exists(outdir / GsplatRadFileName)) << "no tool -> no model.rad";
    EXPECT_FALSE(fs::exists(outdir / GsplatFileName)) << "no tool -> no intermediate model.spz";
}

// A PLY build-lod would refuse fails the build from its header, before the tool is looked for or
// launched (here the tool is missing: the message is still the problem of the file, not the tool's),
// and nothing is written.
TEST(gsplat, buildRefusesASplatPlyBuildLodCannotReadBeforeLaunchingIt) {
    TestArea ta(TEST_NAME);
    const std::string missing = (ta.getFolder("nope") / "build-lod-missing").string();
    BuildLodEnvRestorer guard("DDB_BUILDLOD_PATH", missing.c_str());
    ASSERT_FALSE(ddb::buildlod::isBuildLodAvailable());

    const fs::path ascii = ta.getPath("ascii.ply");
    writeAsciiSplatPly(ascii, 40);
    const fs::path outdir = ta.getPath("gsplat_out");
    const std::string asciiFailure = buildFailure(ascii, outdir);
    EXPECT_NE(asciiFailure.find("ASCII splat PLY is not supported: export it as binary little-endian"),
              std::string::npos)
        << asciiFailure;
    EXPECT_NE(asciiFailure.find(ascii.string()), std::string::npos) << "the message names the file: " << asciiFailure;
    EXPECT_FALSE(fs::exists(outdir)) << "a refused input leaves nothing behind";

    const fs::path odd = ta.getPath("odd.ply");
    writeHeaderOnlyPly(odd, splatHeaderLines("binary_little_endian 1.0", 12));
    EXPECT_NE(buildFailure(odd, outdir).find("f_rest count 12 is not 0, 9, 24 or 45"), std::string::npos);

    const fs::path noOpacity = ta.getPath("no_opacity.ply");
    writeHeaderOnlyPly(noOpacity, splatHeaderLines("binary_little_endian 1.0", 0, "opacity"));
    EXPECT_NE(buildFailure(noOpacity, outdir).find("missing properties: opacity"), std::string::npos);

    // A splat PLY that is fine reaches the tool, which is then the missing one
    const fs::path good = ta.getPath("good.ply");
    writeSplatPly(good, 8, 0);
    expectMissingToolFailure([&] { ddb::buildGsplat(good.string(), outdir.string()); }, "build-lod");
}

// bounds.json is an optional sidecar (the viewer frames the scene without it): a model.rad whose
// centres cannot be decoded, or a bounds.json that cannot be written, does not fail the build or
// leave a part of the sidecar behind.
TEST(gsplat, aFailureToWriteTheBoundsDoesNotFailTheBuild) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("scene.ply");
    writeSplatPly(ply, 80, 1);

#ifndef _WIN32
    {
        // A build-lod that exits 0 with a .rad the reader cannot decode
        const auto tool = testtools::writeFakeTool(
            ta, "build-lod", "printf 'RAD0\\0\\0\\0\\0' > \"$(dirname \"$1\")/source-lod.rad\"\n");
        BuildLodEnvRestorer guard("DDB_BUILDLOD_PATH", tool.string().c_str());
        ASSERT_TRUE(ddb::buildlod::isBuildLodAvailable());

        const fs::path outdir = ta.getFolder("undecodable");
        EXPECT_NO_THROW(ddb::buildGsplat(ply.string(), outdir.string()));
        EXPECT_TRUE(fs::exists(outdir / GsplatRadFileName)) << "the model.rad is delivered";
        EXPECT_FALSE(fs::exists(outdir / GsplatBoundsFileName));
        EXPECT_EQ(namesIn(outdir), (std::vector<std::string>{GsplatRadFileName}));
    }
#endif

    REQUIRE_BUILDLOD_OR_SKIP("build-lod not discoverable; skipping the unwritable bounds assertions");

    // The name of bounds.json is taken by a folder
    const fs::path outdir = ta.getFolder("unwritable");
    fs::create_directory(outdir / GsplatBoundsFileName);
    EXPECT_NO_THROW(ddb::buildGsplat(ply.string(), outdir.string()));
    EXPECT_TRUE(isRad(outdir / GsplatRadFileName));
    EXPECT_TRUE(fs::is_directory(outdir / GsplatBoundsFileName));
    EXPECT_EQ(namesIn(outdir), (std::vector<std::string>{GsplatBoundsFileName, GsplatRadFileName}))
        << "no temporary file of the failed write is left";
}

// A SuperSplat compressed PLY is a splat build-lod reads: the build gives a model.rad of its splats
TEST(gsplat, aCompressedSplatPlyBuilds) {
    TestArea ta(TEST_NAME);
    REQUIRE_BUILDLOD_OR_SKIP("build-lod not discoverable; skipping compressed PLY build");

    const fs::path ply = ta.getPath("compressed.ply");
    writeCompressedSplatPly(ply, 300);
    const fs::path outdir = ta.getFolder("gsplat_out");
    ddb::buildGsplat(ply.string(), outdir.string());

    ASSERT_TRUE(fs::exists(outdir / GsplatRadFileName));
    EXPECT_TRUE(isRad(outdir / GsplatRadFileName));
    EXPECT_TRUE(fs::exists(outdir / GsplatBoundsFileName));
}

// Through the build pipeline: a GaussianSplat entry without build-lod fails the build
// (no ".pending" marker, the C API reports an error rather than a deferral).
TEST(gsplat, buildWithoutBuildLodFailsWithoutPending) {
    TestArea ta(TEST_NAME, true);
    const fs::path dsFolder = ta.getFolder("");
    const fs::path ply = ta.getPath("scene.ply");
    writeSplatPly(ply, 64, 0);

    ddb::initIndex(dsFolder.string());
    auto db = ddb::open(dsFolder.string(), true);
    ddb::addToIndex(db.get(), {ply.string()});
    const auto rel = fs::relative(ply, dsFolder).generic_string();
    Entry e;
    ASSERT_TRUE(loadEntry(db.get(), rel, e));
    ASSERT_EQ(e.type, EntryType::GaussianSplat);

    const std::string missing = (ta.getFolder("nope") / "build-lod-missing").string();
    BuildLodEnvRestorer guard("DDB_BUILDLOD_PATH", missing.c_str());
    ASSERT_FALSE(ddb::buildlod::isBuildLodAvailable());

    expectMissingToolFailure([&] { ddb::build(db.get(), rel, "", false); }, "build-lod");
    EXPECT_FALSE(fs::exists(db->buildDirectory() / (e.hash + ".pending")));
    EXPECT_FALSE(ddb::isBuildPending(db.get()));
    EXPECT_FALSE(ddb::isBuildComplete(db.get(), rel));

    EXPECT_EQ(DDBBuild(dsFolder.string().c_str(), rel.c_str(), nullptr, false, false),
              DDBERR_EXCEPTION);
    EXPECT_FALSE(ddb::isBuildPending(db.get()));
}

// A .ksplat is built by build-lod and read for its header; it is not converted to .spz, and the
// refusal names the formats that are, not a tool that was never involved
TEST(gsplat, ksplatIsNotConvertedToSpzAndTheMessageSaysWhichFormatsAre) {
    TestArea ta(TEST_NAME);
    const fs::path ksplat = ta.getPath("scene.ksplat");
    { std::ofstream o(ksplat.string(), std::ios::binary); o << "ksplatdata"; }

    const fs::path spz = ta.getPath("scene.spz");
    try {
        ddb::convertToSpz(ksplat.string(), spz.string());
        FAIL() << "a .ksplat must not be converted";
    } catch (const InvalidArgsException& e) {
        const std::string what = e.what();
        EXPECT_EQ(what.find("splat-transform"), std::string::npos) << what;
        EXPECT_NE(what.find(".ply, .splat or .spz"), std::string::npos) << what;
    }
    EXPECT_FALSE(fs::exists(spz));
}

// A .ksplat header: version 0.1, the section count and splat count, then one 1024-byte header per
// section whose SH degree sits at byte 40. Sections hold no data here: only the header is read.
void writeKsplatHeader(const fs::path& path, uint32_t splats, const std::vector<uint16_t>& sectionShDegrees,
                       uint8_t minor = 1) {
    std::string header(4096 + sectionShDegrees.size() * 1024, '\0');
    header[0] = 0;
    header[1] = static_cast<char>(minor);
    const uint32_t sections = static_cast<uint32_t>(sectionShDegrees.size());
    std::memcpy(&header[4], &sections, sizeof(sections));
    std::memcpy(&header[16], &splats, sizeof(splats));
    for (size_t i = 0; i < sectionShDegrees.size(); ++i)
        std::memcpy(&header[4096 + i * 1024 + 40], &sectionShDegrees[i], sizeof(uint16_t));
    std::ofstream o(path.string(), std::ios::binary);
    o.write(header.data(), static_cast<std::streamsize>(header.size()));
}

TEST(gsplat, ksplatMetadataIsReadFromItsHeader) {
    TestArea ta(TEST_NAME);

    // The SH degree of the file is the highest of its sections
    const fs::path ksplat = ta.getPath("scene.ksplat");
    writeKsplatHeader(ksplat, 123456, {1, 3, 2});
    GaussianSplatInfo info;
    ASSERT_TRUE(ddb::getGaussianSplatInfo(ksplat.string(), info));
    EXPECT_EQ(info.splatCount, 123456u);
    EXPECT_EQ(info.shDegree, 3);
    EXPECT_TRUE(ddb::looksLikeSplat(ksplat));

    // It is what the entry's properties carry
    json j;
    to_json(j, info);
    EXPECT_EQ(j["splatCount"], 123456);
    EXPECT_EQ(j["shDegree"], 3);

    // Not a .ksplat: the entry stays a splat (the extension decides) with no metadata
    const fs::path junk = ta.getPath("junk.ksplat");
    { std::ofstream o(junk.string(), std::ios::binary); o << "anything"; }
    EXPECT_FALSE(ddb::getGaussianSplatInfo(junk.string(), info));
    EXPECT_FALSE(ddb::looksLikeSplat(junk));
    EXPECT_EQ(ddb::fingerprint(junk), EntryType::GaussianSplat);

    // A version this does not read, no splats, and sections the file is too short to hold
    const fs::path oldVersion = ta.getPath("old.ksplat");
    writeKsplatHeader(oldVersion, 10, {0}, /*minor=*/0);
    EXPECT_FALSE(ddb::getGaussianSplatInfo(oldVersion.string(), info));
    const fs::path empty = ta.getPath("empty.ksplat");
    writeKsplatHeader(empty, 0, {0});
    EXPECT_FALSE(ddb::getGaussianSplatInfo(empty.string(), info));
    const fs::path cut = ta.getPath("cut.ksplat");
    writeKsplatHeader(cut, 10, {0, 0, 0});
    fs::resize_file(cut, 4096 + 1024);
    EXPECT_FALSE(ddb::getGaussianSplatInfo(cut.string(), info));
}

TEST(gsplat, nonSplatPlyConversionThrows) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("cloud.ply");
    writePointCloudPly(ply, 10);
    const fs::path spz = ta.getPath("cloud.spz");
    EXPECT_THROW(ddb::convertToSpz(ply.string(), spz.string()), AppException);
}

// ---------------------------------------------------------------------------
// SPZ format versions (v4/NGSP transcode, legacy gzip copy, truncation contract)
// ---------------------------------------------------------------------------

TEST(gsplat, convertV4SpzTranscodesToV3) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("scene.ply");
    writeSplatPly(ply, 64, 1);

    const fs::path v4 = ta.getPath("scene_v4.spz");
    ASSERT_TRUE(writeSpzVersion(ply, v4, 4));
    ASSERT_EQ(magic4(v4), "NGSP") << "v4 must use the ZSTD 'NGSP' container";

    const fs::path out = ta.getPath("out.spz");
    ddb::convertToSpz(v4.string(), out.string());

    EXPECT_TRUE(isGzip(out)) << "v4/NGSP must be transcoded to gzip-based v3 for the Spark viewer";
    EXPECT_TRUE(ddb::looksLikeSplat(out));

    GaussianSplatInfo srcInfo;
    GaussianSplatInfo outInfo;
    ASSERT_TRUE(ddb::getGaussianSplatInfo(v4.string(), srcInfo));
    ASSERT_TRUE(ddb::getGaussianSplatInfo(out.string(), outInfo));
    EXPECT_EQ(srcInfo.splatCount, 64u);
    EXPECT_EQ(outInfo.splatCount, 64u);
    EXPECT_EQ(srcInfo.shDegree, 1);
    EXPECT_EQ(outInfo.shDegree, 1);
}

TEST(gsplat, getInfoFromV4Spz) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("scene.ply");
    writeSplatPly(ply, 64, 1);

    const fs::path v4 = ta.getPath("scene_v4.spz");
    ASSERT_TRUE(writeSpzVersion(ply, v4, 4));

    // Exercises the NGSP fast header path in readSpzInfo: numPoints/shDegree are read straight
    // from the fixed 32-byte header without decoding the ZSTD streams.
    GaussianSplatInfo info;
    ASSERT_TRUE(ddb::getGaussianSplatInfo(v4.string(), info));
    EXPECT_EQ(info.splatCount, 64u);
    EXPECT_EQ(info.shDegree, 1);
}

TEST(gsplat, legacySpzCopiedAsIs) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("scene.ply");
    writeSplatPly(ply, 64, 1);

    // PackOptions supports the legacy gzip versions directly; v2 exercises the oldest
    // commonly-written container (no smallest-three quaternions).
    const fs::path legacy = ta.getPath("legacy_v2.spz");
    ASSERT_TRUE(writeSpzVersion(ply, legacy, 2));
    EXPECT_TRUE(isGzip(legacy)) << "PackOptions{version=2} must emit the gzip container";

    // convertToSpz passes gzip-based SPZ through unchanged (already viewer-compatible).
    const fs::path copy = ta.getPath("copy.spz");
    ddb::convertToSpz(legacy.string(), copy.string());
    ASSERT_TRUE(fs::exists(copy));
    EXPECT_EQ(fileSize(copy), fileSize(legacy));

    // The legacy file must stay readable through the loadSpzPacked path.
    GaussianSplatInfo info;
    ASSERT_TRUE(ddb::getGaussianSplatInfo(legacy.string(), info));
    EXPECT_EQ(info.splatCount, 64u);
    EXPECT_EQ(info.shDegree, 1);
}

TEST(gsplat, spzPositionsRoundTrip) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("scene.ply");
    writeSplatPly(ply, 64, 1);

    const fs::path spzPath = ta.getPath("scene.spz");
    ddb::convertToSpz(ply.string(), spzPath.string());
    ASSERT_TRUE(fs::exists(spzPath));

    // Decode via the vendored codec and compare against the writeSplatPly layout formula.
    const spz::GaussianCloud cloud = spz::loadSpz(spzPath.string(), spz::UnpackOptions{});
    ASSERT_EQ(cloud.numPoints, 64);
    ASSERT_EQ(cloud.positions.size(), 64u * 3);

    // SPZ stores positions as 24-bit fixed point with fractionalBits=12 (vendor/spz
    // load-spz.cc packGaussians), so the quantization step is 2^-12 ~= 0.00024; 0.05 gives
    // generous rounding slack while still catching coordinate-handling or scale regressions.
    constexpr float kTol = 0.05f;
    for (int i = 0; i < 64; ++i) {
        const size_t b = static_cast<size_t>(i) * 3;
        EXPECT_NEAR(static_cast<float>(i % 10), cloud.positions[b], kTol) << "x i=" << i;
        EXPECT_NEAR(static_cast<float>(i / 10), cloud.positions[b + 1], kTol) << "y i=" << i;
        EXPECT_NEAR(static_cast<float>(i % 3) * 0.1f, cloud.positions[b + 2], kTol) << "z i=" << i;
    }
}

// Locks the CURRENT contract for half-truncated SPZ files: the vendored spz never throws on
// corrupt input, it returns an empty cloud, so (convertToSpz, getGaussianSplatInfo):
//  - gzip (v3): the gzip pass-through branch performs no validation and copies the broken
//    bytes as-is.
//  - NGSP (v4): the transcode path throws AppException (nothing decoded).
//  - Both: getGaussianSplatInfo reads the header alone (indexing must stay cheap), so it still
//    reports the splat count the header declares. Any real decode of the truncated file fails,
//    so nothing downstream takes the count for splats that can be read.
TEST(gsplat, truncatedSpzRejected) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("scene.ply");
    writeSplatPly(ply, 64, 1);
    const fs::path v3 = ta.getPath("scene.spz");
    ddb::convertToSpz(ply.string(), v3.string());
    const fs::path v4 = ta.getPath("scene_v4.spz");
    ASSERT_TRUE(writeSpzVersion(ply, v4, 4));

    const auto truncateToHalf = [](const fs::path& src, const fs::path& dst) {
        std::ifstream in(src.string(), std::ios::binary | std::ios::ate);
        ASSERT_TRUE(in.good());
        const auto half = static_cast<size_t>(in.tellg()) / 2;
        std::vector<uint8_t> b(half);
        in.seekg(0);
        in.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(half));
        std::ofstream out(dst.string(), std::ios::binary);
        out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    };
    const fs::path halfV3 = ta.getPath("half_v3.spz");
    const fs::path halfV4 = ta.getPath("half_v4.spz");
    truncateToHalf(v3, halfV3);
    truncateToHalf(v4, halfV4);

    const fs::path out3 = ta.getPath("out3.spz");
    ddb::convertToSpz(halfV3.string(), out3.string());
    EXPECT_EQ(fileSize(out3), fileSize(halfV3));
    GaussianSplatInfo info;
    ASSERT_TRUE(ddb::getGaussianSplatInfo(halfV3.string(), info));
    EXPECT_EQ(info.splatCount, 64u);
    ASSERT_TRUE(ddb::getGaussianSplatInfo(out3.string(), info));
    EXPECT_EQ(info.splatCount, 64u);
    EXPECT_TRUE(spz::loadSpz(halfV3.string(), spz::UnpackOptions{}).positions.empty())
        << "the truncated stream itself decodes to nothing";

    const fs::path out4 = ta.getPath("out4.spz");
    EXPECT_THROW(ddb::convertToSpz(halfV4.string(), out4.string()), AppException);
    GaussianSplatInfo info4;
    EXPECT_TRUE(ddb::getGaussianSplatInfo(halfV4.string(), info4));
    EXPECT_EQ(info4.splatCount, 64u);
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

// generateThumbs ("ddb thumbs") renders every file a thumbnail generator of the entry handler
// registry accepts, as generateThumb (DDBThumbnail) does: a .spz is no longer skipped.
TEST(gsplat, generateThumbsRendersSpz) {
    TestArea ta(TEST_NAME, true);
    const fs::path ply = ta.getPath("scene.ply");
    writeSplatPly(ply, 400, 1);
    const fs::path spz = ta.getPath("scene.spz");
    ddb::convertToSpz(ply.string(), spz.string());
    ASSERT_TRUE(fs::exists(spz));

    const fs::path outdir = ta.getFolder("thumbs");
    ddb::generateThumbs({spz.string()}, outdir, 128, false);

    const fs::path thumb = outdir / "scene.webp";
    ASSERT_TRUE(fs::exists(thumb));
    EXPECT_TRUE(isWebp(thumb));
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
    REQUIRE_BUILDLOD_OR_SKIP("build-lod not discoverable; Gaussian Splat builds require it");

    ddb::initIndex(dsFolder.string());
    auto db = ddb::open(dsFolder.string(), true);
    ddb::addToIndex(db.get(), {ply.string()});

    const auto rel = fs::relative(ply, dsFolder).string();

    Entry e;
    ASSERT_TRUE(loadEntry(db.get(), rel, e));
    EXPECT_EQ(e.type, EntryType::GaussianSplat);

    EXPECT_EQ(ddb::buildSubfolder(db.get(), rel), "gsplat");

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
    // the build temp folder is published as is: no staged input or scratch folder in it
    EXPECT_EQ(namesIn(gsplatDir), (std::vector<std::string>{GsplatBoundsFileName, GsplatRadFileName}));
}

// A build made before the RAD layout left gsplat/model.spz; gsplat/ also holds the model.rad that
// replaces it, so the cleanup that follows a build removes that file alone and never the folder
TEST(gsplat, aBuildRemovesAnOldModelSpzAndKeepsTheModelRadBesideIt) {
    TestArea ta(TEST_NAME, true);
    REQUIRE_BUILDLOD_OR_SKIP("build-lod not discoverable; Gaussian Splat builds require it");

    const fs::path dsFolder = ta.getFolder("");
    const fs::path ply = ta.getPath("scene.ply");
    writeSplatPly(ply, 80, 1);
    ddb::initIndex(dsFolder.string());
    auto db = ddb::open(dsFolder.string(), true);
    ddb::addToIndex(db.get(), {ply.string()});
    const auto rel = fs::relative(ply, dsFolder).string();
    Entry e;
    ASSERT_TRUE(loadEntry(db.get(), rel, e));

    const fs::path gsplatDir = fs::path(db->buildDirectory()) / e.hash / "gsplat";
    const auto expectCurrentOutput = [&] {
        EXPECT_TRUE(isRad(gsplatDir / GsplatRadFileName)) << "model.rad survives the cleanup";
        EXPECT_TRUE(fs::exists(gsplatDir / GsplatBoundsFileName));
        EXPECT_TRUE(ddb::isBuildComplete(db.get(), rel));
    };

    ddb::build(db.get(), rel, "", false);
    expectCurrentOutput();

    // The output of an older build is still there when the entry is built again: the output
    // is complete, the build is skipped and the old model.spz is cleaned up
    {
        std::ofstream((gsplatDir / GsplatFileName).string(), std::ios::binary) << "old SPZ";
    }
    ASSERT_TRUE(fs::exists(gsplatDir / GsplatFileName));
    ddb::build(db.get(), rel, "", false);
    EXPECT_FALSE(fs::exists(gsplatDir / GsplatFileName)) << "the superseded model.spz is removed";
    expectCurrentOutput();
    EXPECT_EQ(namesIn(gsplatDir), (std::vector<std::string>{GsplatBoundsFileName, GsplatRadFileName}));
}

// ---------------------------------------------------------------------------
// Formats beyond the first three: .sog and .rad sources, the header-only readers
// ---------------------------------------------------------------------------

// A SOGS v2 bundle of four splats: a ZIP holding meta.json and five lossless 2x2 WebP images (means
// in two bytes, scales, quaternions, colour and opacity). The one bytes-in-the-test fixture of a
// format this repository has no writer for.
void writeSogBundle(const fs::path& path) {
    static const char* hex =
    "504b0304140000000000411d5c5da4fa24d91a0100001a010000090000006d6574612e6a736f6e7b2276657273696f6e"
    "223a20322c2022636f756e74223a20342c20226d65616e73223a207b2266696c6573223a205b226d65616e735f6c2e77"
    "656270222c20226d65616e735f752e77656270225d2c20226d696e73223a205b2d312c202d312c202d315d2c20226d61"
    "7873223a205b312c20312c20315d7d2c20227363616c6573223a207b2266696c6573223a205b227363616c65732e7765"
    "6270225d2c2022636f6465626f6f6b223a205b2d342e305d7d2c20227175617473223a207b2266696c6573223a205b22"
    "71756174732e77656270225d7d2c2022736830223a207b2266696c6573223a205b227368302e77656270225d2c202263"
    "6f6465626f6f6b223a205b2d312e302c20302e302c20312e302c20322e305d7d7d504b0304140000000000411d5c5dcc"
    "fad69d26000000260000000c0000006d65616e735f6c2e77656270524946461e000000574542505650384c110000002f"
    "0140000007d0fffef7bfff8188e87f0000504b0304140000000000411d5c5d2fb0088938000000380000000c0000006d"
    "65616e735f752e776562705249464630000000574542505650384c230000002f014000001f201048f166ab251048325b"
    "6df4f99bffb8a10128091000451989e87f0c00504b0304140000000000411d5c5d3812562d22000000220000000b0000"
    "007363616c65732e77656270524946461a000000574542505650384c0e0000002f01400000071011fd0f4444ff03504b"
    "0304140000000000411d5c5de5c19eb826000000260000000a00000071756174732e77656270524946461e0000005745"
    "42505650384c110000002f014000000750c00216b0ff8188e87f0000504b0304140000000000411d5c5d066712f83600"
    "000036000000080000007368302e77656270524946462e000000574542505650384c210000002f014000101fa0206d03"
    "66c7bfe6d898801118e4b84504640394a9c22ca2ffb10300504b01021403140000000000411d5c5da4fa24d91a010000"
    "1a0100000900000000000000000000008001000000006d6574612e6a736f6e504b01021403140000000000411d5c5dcc"
    "fad69d26000000260000000c00000000000000000000008001410100006d65616e735f6c2e77656270504b0102140314"
    "0000000000411d5c5d2fb0088938000000380000000c00000000000000000000008001910100006d65616e735f752e77"
    "656270504b01021403140000000000411d5c5d3812562d22000000220000000b00000000000000000000008001f30100"
    "007363616c65732e77656270504b01021403140000000000411d5c5de5c19eb826000000260000000a00000000000000"
    "0000000080013e02000071756174732e77656270504b01021403140000000000411d5c5d066712f83600000036000000"
    "08000000000000000000000080018c0200007368302e77656270504b0506000000000600060052010000e80200000000";
    std::string bytes;
    for (const char* c = hex; *c != '\0'; c += 2)
        bytes.push_back(static_cast<char>(std::stoi(std::string(c, 2), nullptr, 16)));
    std::ofstream out(path.string(), std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// build-lod reads a single-file .sog: the entry is detected, built and thumbnailed like any splat,
// and has no splat count because the count is inside the ZIP, which indexing does not open
TEST(gsplat, aSogBundleIsDetectedBuiltAndThumbnailed) {
    TestArea ta(TEST_NAME);
    const fs::path sog = ta.getPath("scene.sog");
    writeSogBundle(sog);

    EXPECT_EQ(ddb::fingerprint(sog), EntryType::GaussianSplat);
    EXPECT_TRUE(ddb::looksLikeSplat(sog));
    GaussianSplatInfo info;
    EXPECT_FALSE(ddb::getGaussianSplatInfo(sog.string(), info)) << "a .sog has no metadata reader";

    // Not a ZIP: still a splat by its extension, but its content is not what it says
    const fs::path junk = ta.getPath("junk.sog");
    { std::ofstream o(junk.string(), std::ios::binary); o << "not a zip"; }
    EXPECT_EQ(ddb::fingerprint(junk), EntryType::GaussianSplat);
    EXPECT_FALSE(ddb::looksLikeSplat(junk));

    REQUIRE_BUILDLOD_OR_SKIP("build-lod not discoverable; skipping the .sog build");
    const fs::path outdir = ta.getFolder("gsplat_out");
    ddb::buildGsplat(sog.string(), outdir.string());
    const fs::path modelRad = outdir / GsplatRadFileName;
    ASSERT_TRUE(isRad(modelRad)) << "build-lod turns the bundle into model.rad";
    EXPECT_EQ(namesIn(outdir), (std::vector<std::string>{GsplatBoundsFileName, GsplatRadFileName}));

    const fs::path thumb = ta.getPath("thumb.webp");
    ddb::generateThumb(modelRad, 128, thumb, true);
    EXPECT_TRUE(isWebp(thumb));
}

// A vertex count that does not fit in 32 bits (an unsigned long is 32 bits on Windows) is kept whole
TEST(gsplat, aPlyWithMoreThan2To32VerticesKeepsItsCount) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("huge.ply");
    auto lines = splatHeaderLines();
    lines[1] = "element vertex 5000000000";
    writeHeaderOnlyPly(ply, lines);

    PlyInfo plyInfo;
    ASSERT_TRUE(ddb::getPlyInfo(ply, plyInfo));
    EXPECT_EQ(plyInfo.vertexCount, uint64_t{5000000000});

    GaussianSplatInfo info;
    ASSERT_TRUE(ddb::getGaussianSplatInfo(ply.string(), info));
    EXPECT_EQ(info.splatCount, uint64_t{5000000000});
}

// SPZ keeps the number of splats in an int32_t: more are refused with the count, not wrapped
TEST(gsplat, aSplatCountSpzCannotHoldIsRefused) {
    EXPECT_EQ(ddb::spzPointCount(0, "a.splat"), 0);
    EXPECT_EQ(ddb::spzPointCount(INT32_MAX, "a.splat"), INT32_MAX);
    try {
        ddb::spzPointCount(uint64_t{INT32_MAX} + 1, "big.splat");
        FAIL() << "2^31 splats do not fit";
    } catch (const AppException& e) {
        const std::string what = e.what();
        EXPECT_NE(what.find("big.splat"), std::string::npos) << what;
        EXPECT_NE(what.find("2147483648"), std::string::npos) << what;
        EXPECT_NE(what.find("2147483647"), std::string::npos) << what;
    }
}

std::string gzipOf(const std::string& data) {
    z_stream strm{};
    EXPECT_EQ(deflateInit2(&strm, Z_BEST_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY), Z_OK);
    std::string out;
    strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    strm.avail_in = static_cast<uInt>(data.size());
    char buffer[64 * 1024];
    int ret;
    do {
        strm.next_out = reinterpret_cast<Bytef*>(buffer);
        strm.avail_out = sizeof(buffer);
        ret = deflate(&strm, Z_FINISH);
        out.append(buffer, sizeof(buffer) - strm.avail_out);
    } while (ret != Z_STREAM_END);
    deflateEnd(&strm);
    return out;
}

// The 16-byte header of a legacy SPZ: magic, version, splat count, SH degree, fractional bits, flags
std::string spzHeaderBytes(uint32_t version, uint32_t splats, uint8_t shDegree) {
    std::string h("NGSP", 4);
    h.append(reinterpret_cast<const char*>(&version), 4);
    h.append(reinterpret_cast<const char*>(&splats), 4);
    h.push_back(static_cast<char>(shDegree));
    h.append(3, '\0');
    return h;
}

void writeBytes(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path.string(), std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// Indexing reads the header of a legacy SPZ, never its splats: only the first 16 bytes of the gzip
// stream are inflated. A stream cut off right after its start still yields the header (a reader that
// inflates the whole file cannot), and a body that is not a splat stream is never looked at.
TEST(gsplat, aLegacySpzHeaderIsReadWithoutInflatingTheFile) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("scene.ply");
    writeSplatPly(ply, 300, 2);
    const fs::path legacy = ta.getPath("legacy.spz");
    ASSERT_TRUE(writeSpzVersion(ply, legacy, 3));
    ASSERT_TRUE(isGzip(legacy));
    const std::string whole = readText(legacy);
    ASSERT_GT(whole.size(), 400u);

    // Cut short after the first few hundred bytes: the splats cannot be decoded, the header can
    const fs::path cut = ta.getPath("cut.spz");
    writeBytes(cut, whole.substr(0, 200));
    ASSERT_TRUE(spz::loadSpz(cut.string(), spz::UnpackOptions{}).positions.empty());
    GaussianSplatInfo info;
    ASSERT_TRUE(ddb::getGaussianSplatInfo(cut.string(), info));
    EXPECT_EQ(info.splatCount, 300u);
    EXPECT_EQ(info.shDegree, 2);

    // A header followed by far more than a test wants to inflate: 64 MiB of zeros, tens of
    // KiB of gzip. The count is the header's, the file is not looked at past it.
    const fs::path big = ta.getPath("big.spz");
    writeBytes(big, gzipOf(spzHeaderBytes(3, 7, 1) + std::string(size_t{64} << 20, '\0')));
    ASSERT_TRUE(ddb::getGaussianSplatInfo(big.string(), info));
    EXPECT_EQ(info.splatCount, 7u);
    EXPECT_EQ(info.shDegree, 1);
    SpzHeader header;
    ASSERT_TRUE(ddb::readSpzHeader(big, header));
    EXPECT_EQ(header.container, SpzContainer::Gzip);
    EXPECT_EQ(header.version, 3u);

    // A gzip stream that is not an SPZ, one with no splats, and one too short for a header
    const fs::path notSpz = ta.getPath("gzip.spz");
    writeBytes(notSpz, gzipOf("this is a gzip file but not a splat"));
    EXPECT_FALSE(ddb::getGaussianSplatInfo(notSpz.string(), info));
    const fs::path none = ta.getPath("none.spz");
    writeBytes(none, gzipOf(spzHeaderBytes(3, 0, 0)));
    EXPECT_FALSE(ddb::getGaussianSplatInfo(none.string(), info));
    const fs::path tiny = ta.getPath("tiny.spz");
    writeBytes(tiny, gzipOf("NGSP"));
    EXPECT_FALSE(ddb::getGaussianSplatInfo(tiny.string(), info));
}

// The STAC data asset of a splat has the media type the format table (and the build manifest) names
TEST(gsplat, theStacAssetOfASplatHasTheMediaTypeOfTheFormatTable) {
    TestArea ta(TEST_NAME, true);
    writeSplatBinary(ta.getFolder() / "scene.splat", 4);
    writeSogBundle(ta.getFolder() / "bundle.sog");
    ddb::initIndex(ta.getFolder().string());
    auto db = ddb::open(ta.getFolder().string(), true);
    ddb::addToIndex(db.get(), {(ta.getFolder() / "scene.splat").string(), (ta.getFolder() / "bundle.sog").string()});

    for (const char* name : {"scene.splat", "bundle.sog"}) {
        const auto item = ddb::generateStac(ta.getFolder().string(), name);
        ASSERT_TRUE(item["assets"].contains(name)) << item.dump();
        EXPECT_EQ(item["assets"][name]["type"], GsplatMediaType) << name;
    }
}

// A .rad is what a build makes: an upload of one is a splat that is checked and published as it is,
// without build-lod (WebODM tasks produce a splats.rad), so it builds with no build-lod at all
TEST(gsplat, aRadUploadIsPublishedWithoutBuildLod) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("scene.ply");
    writeSplatPly(ply, 400, 1);

    // A RAD as a trainer delivers it, made here by build-lod
    REQUIRE_BUILDLOD_OR_SKIP("build-lod not discoverable; skipping the RAD upload test");
    const fs::path made = ta.getFolder("made");
    ddb::buildGsplat(ply.string(), made.string());
    const fs::path upload = ta.getPath("splats.rad");
    fs::copy_file(made / GsplatRadFileName, upload);
    const std::string content = readText(upload);

    // build-lod is not there, or does not work when it is: it is not launched
    const std::string missing = (ta.getFolder("nope") / "build-lod-missing").string();
    BuildLodEnvRestorer guard("DDB_BUILDLOD_PATH", missing.c_str());
    ASSERT_FALSE(ddb::buildlod::isBuildLodAvailable());
    EXPECT_EQ(ddb::fingerprint(upload), EntryType::GaussianSplat);

    const fs::path outdir = ta.getPath("published");
    ASSERT_NO_THROW(ddb::buildGsplat(upload.string(), outdir.string()));

    // The folder holds the model and its sidecar, nothing else; the model is the file as it came
    EXPECT_EQ(namesIn(outdir), (std::vector<std::string>{GsplatBoundsFileName, GsplatRadFileName}));
    EXPECT_EQ(readText(outdir / GsplatRadFileName), content);
    EXPECT_EQ(readText(upload), content) << "the dataset file is not touched";

    // The sidecar is that of any build: the box, and which way is up
    const json bounds = json::parse(readText(outdir / GsplatBoundsFileName));
    ASSERT_TRUE(bounds.contains("min") && bounds.contains("max"));
    EXPECT_EQ(bounds["max"].size(), 3u);
    EXPECT_TRUE(bounds.contains("up") && bounds.contains("upConfidence") && bounds.contains("upSource"));
    std::array<double, 3> bmin{}, bmax{};
    ASSERT_TRUE(ddb::computeRadBounds(upload, bmin, bmax));
    for (int k = 0; k < 3; ++k) {
        EXPECT_DOUBLE_EQ(bounds["min"][k].get<double>(), bmin[k]);
        EXPECT_DOUBLE_EQ(bounds["max"][k].get<double>(), bmax[k]);
    }

#ifndef _WIN32
    // And a build-lod that is there and must not be called
    const fs::path called = ta.getPath("build-lod-called");
    const auto tool = testtools::writeFakeTool(ta, "build-lod", "touch '" + called.string() + "'\nexit 1\n");
    BuildLodEnvRestorer fake("DDB_BUILDLOD_PATH", tool.string().c_str());
    ASSERT_TRUE(ddb::buildlod::isBuildLodAvailable());
    const fs::path second = ta.getPath("published_again");
    EXPECT_NO_THROW(ddb::buildGsplat(upload.string(), second.string()));
    EXPECT_FALSE(fs::exists(called)) << "build-lod was launched for a .rad";
    EXPECT_TRUE(isRad(second / GsplatRadFileName));
#endif
}

// A .rad that is not one fails the build with the reason, and nothing is published
TEST(gsplat, aCorruptRadFailsTheBuildWithTheReason) {
    TestArea ta(TEST_NAME);
    const std::string missing = (ta.getFolder("nope") / "build-lod-missing").string();
    BuildLodEnvRestorer guard("DDB_BUILDLOD_PATH", missing.c_str());

    const std::string header = std::string("RAD0") + std::string("\x02\0\0\0", 4);
    const std::vector<std::pair<std::string, std::string>> corrupt = {
        {"magic.rad", "PK\x03\x04 this is a ZIP"},
        {"empty_meta.rad", header + "{}\0\0\0\0\0\0"},
        {"truncated.rad", "RAD0"},
        {"meta_beyond_file.rad", std::string("RAD0") + std::string("\xf0\xff\xff\xff", 4) + "{}"},
    };
    for (const auto& [name, bytes] : corrupt) {
        SCOPED_TRACE(name);
        const fs::path rad = ta.getPath(name);
        writeBytes(rad, bytes);
        const fs::path outdir = ta.getPath("out_" + name);
        const std::string failure = buildFailure(rad, outdir);
        EXPECT_NE(failure.find("Cannot build the Gaussian splat"), std::string::npos) << failure;
        EXPECT_NE(failure.find("RAD"), std::string::npos) << failure;
        EXPECT_FALSE(fs::exists(outdir / GsplatRadFileName));
        EXPECT_FALSE(fs::exists(outdir / GsplatBoundsFileName));
    }
}

// The pipeline: a .rad of a dataset is a GaussianSplat entry and builds as the others do
TEST(gsplat, aRadOfADatasetIsAnEntryThatBuildsWithoutBuildLod) {
    TestArea ta(TEST_NAME, true);
    const fs::path ply = ta.getPath("scene.ply");
    writeSplatPly(ply, 200, 1);
    REQUIRE_BUILDLOD_OR_SKIP("build-lod not discoverable; skipping the RAD upload pipeline test");
    const fs::path made = ta.getFolder("made");
    ddb::buildGsplat(ply.string(), made.string());

    const fs::path dsFolder = ta.getFolder("ds");
    const fs::path rad = dsFolder / "splats.rad";
    fs::copy_file(made / GsplatRadFileName, rad);

    const std::string missing = (ta.getFolder("nope") / "build-lod-missing").string();
    BuildLodEnvRestorer guard("DDB_BUILDLOD_PATH", missing.c_str());

    ddb::initIndex(dsFolder.string());
    auto db = ddb::open(dsFolder.string(), true);
    ddb::addToIndex(db.get(), {rad.string()});
    Entry e;
    ASSERT_TRUE(loadEntry(db.get(), "splats.rad", e));
    EXPECT_EQ(e.type, EntryType::GaussianSplat);
    EXPECT_TRUE(ddb::isBuildable(db.get(), "splats.rad"));

    ddb::build(db.get(), "splats.rad", "", false);
    EXPECT_TRUE(ddb::isBuildComplete(db.get(), "splats.rad"));
    const fs::path gsplatDir = fs::path(db->buildDirectory()) / e.hash / "gsplat";
    EXPECT_EQ(namesIn(gsplatDir), (std::vector<std::string>{GsplatBoundsFileName, GsplatRadFileName}));
}

// A RAD whose splats are readable but whose orientation cannot be decoded still publishes, with a
// bounding box and no up vector: the estimate is optional like the rest of the sidecar
TEST(gsplat, aRadWhoseUpCannotBeEstimatedStillPublishesItsBounds) {
    TestArea ta(TEST_NAME);
    const std::string missing = (ta.getFolder("nope") / "build-lod-missing").string();
    BuildLodEnvRestorer guard("DDB_BUILDLOD_PATH", missing.c_str());

    // Two splats at (1,2,5) and (3,4,7); the orientation uses an encoding nothing reads
    const float centres[6] = {1, 3, 2, 4, 5, 7};
    const std::string payload = std::string(reinterpret_cast<const char*>(centres), sizeof(centres)) + std::string(6, '\0');
    json meta = {{"count", 2},
                 {"properties", json::array({{{"property", "center"}, {"encoding", "f32"}, {"offset", 0}, {"bytes", 24}},
                                             {{"property", "orientation"}, {"encoding", "s8"}, {"offset", 24}, {"bytes", 6}}})}};
    const auto pad8 = [](std::string v) { v.resize((v.size() + 7u) & ~size_t(7), '\0'); return v; };
    const auto le = [](uint64_t v, size_t n) { return std::string(reinterpret_cast<const char*>(&v), n); };
    const std::string chunk = "RADC" + le(meta.dump().size(), 4) + pad8(meta.dump()) + le(payload.size(), 8) + payload;
    const json header = {{"chunks", json::array({{{"offset", 0}, {"bytes", chunk.size()}}})}};
    const fs::path rad = ta.getPath("odd.rad");
    writeBytes(rad, "RAD0" + le(header.dump().size(), 4) + pad8(header.dump()) + chunk);

    const fs::path outdir = ta.getPath("out");
    ASSERT_NO_THROW(ddb::buildGsplat(rad.string(), outdir.string()));
    EXPECT_TRUE(isRad(outdir / GsplatRadFileName));
    const json bounds = json::parse(readText(outdir / GsplatBoundsFileName));
    EXPECT_EQ(bounds["min"], (json::array({1.0, 2.0, 5.0})));
    EXPECT_EQ(bounds["max"], (json::array({3.0, 4.0, 7.0})));
    EXPECT_FALSE(bounds.contains("up"));
}

} // namespace
