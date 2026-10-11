/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Track D-B (converter part): streaming PLY -> OBJ converter (plyconv.h) and the
// shared PLY header model (ply.h).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <gdal.h>
#include <tiny_obj_loader.h>

#include "3d.h"
#include "build.h"
#include "dbops.h"
#include "ddb.h"
#include "entry.h"
#include "exceptions.h"
#include "fs.h"
#include "gtest/gtest.h"
#include "obj2tiles_runner.h"
#include "ply.h"
#include "plyconv.h"
#include "test.h"
#include "testarea.h"

namespace {

using namespace ddb;

// ---------------------------------------------------------------------------
// Generic PLY writer (all formats, all scalar types, lists)
// ---------------------------------------------------------------------------

struct Prop {
    std::string type;       // item type for lists
    std::string name;
    std::string countType;  // non-empty => list property
};

using Row = std::vector<std::vector<double>>;  // one value vector per property (lists: items)

struct Elem {
    std::string name;
    std::vector<Prop> props;
    std::vector<Row> rows;
    // Declared count when different from rows.size() (truncation tests).
    long long declaredCount = -1;
};

struct PlySpec {
    std::string format = "binary_little_endian";
    std::string formatLine;  // overrides the generated `format` line when non-empty
    std::vector<std::string> comments;
    std::vector<Elem> elements;
    bool crlf = false;
    std::string trailingGarbage;  // appended after the body
};

bool hostBigEndian() {
    const uint16_t one = 1;
    unsigned char b;
    std::memcpy(&b, &one, 1);
    return b == 0;
}

template <typename T>
void putScalar(std::string& out, double v, bool bigEndian) {
    T x = static_cast<T>(v);
    unsigned char b[sizeof(T)];
    std::memcpy(b, &x, sizeof(T));
    if (bigEndian != hostBigEndian())
        std::reverse(b, b + sizeof(T));
    out.append(reinterpret_cast<const char*>(b), sizeof(T));
}

void putTyped(std::string& out, const std::string& type, double v, bool be) {
    if (type == "char" || type == "int8") putScalar<int8_t>(out, v, be);
    else if (type == "uchar" || type == "uint8") putScalar<uint8_t>(out, v, be);
    else if (type == "short" || type == "int16") putScalar<int16_t>(out, v, be);
    else if (type == "ushort" || type == "uint16") putScalar<uint16_t>(out, v, be);
    else if (type == "int" || type == "int32") putScalar<int32_t>(out, v, be);
    else if (type == "uint" || type == "uint32") putScalar<uint32_t>(out, v, be);
    else if (type == "float" || type == "float32") putScalar<float>(out, v, be);
    else if (type == "double" || type == "float64") putScalar<double>(out, v, be);
    else throw std::runtime_error("bad test type " + type);
}

std::string asciiNum(double v) {
    std::ostringstream ss;
    ss.imbue(std::locale::classic());
    ss.precision(17);
    ss << v;
    return ss.str();
}

void writePly(const fs::path& path, const PlySpec& spec) {
    const std::string eol = spec.crlf ? "\r\n" : "\n";
    std::string out = "ply" + eol;
    out += (spec.formatLine.empty() ? "format " + spec.format + " 1.0" : spec.formatLine) + eol;
    for (const auto& c : spec.comments)
        out += "comment " + c + eol;
    for (const auto& e : spec.elements) {
        const long long n = e.declaredCount >= 0 ? e.declaredCount : static_cast<long long>(e.rows.size());
        out += "element " + e.name + " " + std::to_string(n) + eol;
        for (const auto& p : e.props) {
            if (p.countType.empty())
                out += "property " + p.type + " " + p.name + eol;
            else
                out += "property list " + p.countType + " " + p.type + " " + p.name + eol;
        }
    }
    out += "end_header" + eol;

    const bool ascii = spec.format == "ascii";
    const bool be = spec.format == "binary_big_endian";
    for (const auto& e : spec.elements) {
        for (const auto& row : e.rows) {
            std::string line;
            for (size_t k = 0; k < e.props.size(); ++k) {
                const Prop& p = e.props[k];
                const auto& vals = row.at(k);
                if (ascii) {
                    if (!line.empty()) line += ' ';
                    if (!p.countType.empty()) line += std::to_string(vals.size()) + (vals.empty() ? "" : " ");
                    for (size_t i = 0; i < vals.size(); ++i) {
                        if (i) line += ' ';
                        line += asciiNum(vals[i]);
                    }
                } else {
                    if (!p.countType.empty()) putTyped(out, p.countType, static_cast<double>(vals.size()), be);
                    for (double v : vals) putTyped(out, p.type, v, be);
                }
            }
            if (ascii) out += line + eol;
        }
    }
    out += spec.trailingGarbage;

    std::ofstream f(path.string(), std::ios::binary | std::ios::trunc);
    f.write(out.data(), static_cast<std::streamsize>(out.size()));
}

using Vec3 = std::array<double, 3>;

Elem vertexElem(const std::vector<Vec3>& pos, const std::string& type = "float") {
    Elem e{"vertex", {{type, "x", ""}, {type, "y", ""}, {type, "z", ""}}, {}};
    for (const auto& p : pos) e.rows.push_back({{p[0]}, {p[1]}, {p[2]}});
    return e;
}

Elem faceElem(const std::vector<std::vector<int>>& faces, const std::string& countType = "uchar",
              const std::string& indexType = "int", const std::string& name = "vertex_indices") {
    Elem e{"face", {{indexType, name, countType}}, {}};
    for (const auto& f : faces) e.rows.push_back({std::vector<double>(f.begin(), f.end())});
    return e;
}

// Unit square (2 triangles), unit square as one quad, regular hexagon (one n-gon).
const std::vector<Vec3> kSquare = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0.5}};
const std::vector<std::vector<int>> kSquareTris = {{0, 1, 2}, {0, 2, 3}};
const std::vector<std::vector<int>> kSquareQuad = {{0, 1, 2, 3}};

std::vector<Vec3> hexagon() {
    std::vector<Vec3> v;
    for (int i = 0; i < 6; ++i) {
        const double a = i * 3.14159265358979323846 / 3.0;
        v.push_back({std::cos(a), std::sin(a), 0.25 * i});
    }
    return v;
}

// Grid of n x n vertices with z = f(x,y); quads or triangles.
std::vector<Vec3> gridVertices(int n) {
    std::vector<Vec3> v;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
            v.push_back({i * 0.5, j * 0.5, std::sin(i * 0.3) * std::cos(j * 0.2)});
    return v;
}

std::vector<std::vector<int>> gridFaces(int n, bool quads) {
    std::vector<std::vector<int>> f;
    for (int j = 0; j + 1 < n; ++j)
        for (int i = 0; i + 1 < n; ++i) {
            const int a = j * n + i, b = a + 1, c = a + n + 1, d = a + n;
            if (quads) {
                f.push_back({a, b, c, d});
            } else {
                f.push_back({a, b, c});
                f.push_back({a, c, d});
            }
        }
    return f;
}

// Writes a small valid JPEG (GDAL JPEG driver), so no binary fixture is committed.
bool writeJpeg(const fs::path& path, int w = 8, int h = 8) {
    GDALDriverH mem = GDALGetDriverByName("MEM");
    GDALDriverH jpg = GDALGetDriverByName("JPEG");
    if (!mem || !jpg) return false;
    GDALDatasetH ds = GDALCreate(mem, "", w, h, 3, GDT_Byte, nullptr);
    if (!ds) return false;
    std::vector<uint8_t> px(static_cast<size_t>(w) * h);
    for (int b = 1; b <= 3; ++b) {
        for (int i = 0; i < w * h; ++i) px[i] = static_cast<uint8_t>((i * 17 * b) & 0xFF);
        if (GDALRasterIO(GDALGetRasterBand(ds, b), GF_Write, 0, 0, w, h, px.data(), w, h, GDT_Byte, 0, 0) != CE_None) {
            GDALClose(ds);
            return false;
        }
    }
    GDALDatasetH out = GDALCreateCopy(jpg, path.string().c_str(), ds, FALSE, nullptr, nullptr, nullptr);
    GDALClose(ds);
    if (!out) return false;
    GDALClose(out);
    std::error_code ec;
    fs::remove(fs::path(path.string() + ".aux.xml"), ec);
    return fs::exists(path);
}

// F1: faces + per-face texcoord lists (MeshLab style) + `comment TextureFile tex.jpg`.
void writeF1(const fs::path& ply, const std::string& textureComment = "TextureFile tex.jpg") {
    const int n = 6;
    PlySpec s;
    s.comments = {"VCGLIB generated", textureComment};
    s.elements.push_back(vertexElem(gridVertices(n)));
    Elem f{"face", {{"int", "vertex_indices", "uchar"}, {"float", "texcoord", "uchar"}}, {}};
    for (const auto& face : gridFaces(n, false)) {
        std::vector<double> tc;
        for (int idx : face) {
            tc.push_back((idx % n) / double(n - 1));
            tc.push_back((idx / n) / double(n - 1));
        }
        f.rows.push_back({std::vector<double>(face.begin(), face.end()), tc});
    }
    s.elements.push_back(f);
    writePly(ply, s);
}

// F2: faces + vertex s,t, no TextureFile comment.
void writeF2(const fs::path& ply) {
    const int n = 6;
    PlySpec s;
    Elem v{"vertex", {{"float", "x", ""}, {"float", "y", ""}, {"float", "z", ""}, {"float", "s", ""}, {"float", "t", ""}}, {}};
    int i = 0;
    for (const auto& p : gridVertices(n)) {
        v.rows.push_back({{p[0]}, {p[1]}, {p[2]}, {(i % n) / double(n - 1)}, {(i / n) / double(n - 1)}});
        ++i;
    }
    s.elements.push_back(v);
    s.elements.push_back(faceElem(gridFaces(n, false)));
    writePly(ply, s);
}

// F3: untextured, uchar red/green/blue.
void writeF3(const fs::path& ply, int n = 8, bool quads = false) {
    PlySpec s;
    Elem v{"vertex",
           {{"float", "x", ""}, {"float", "y", ""}, {"float", "z", ""}, {"uchar", "red", ""}, {"uchar", "green", ""}, {"uchar", "blue", ""}},
           {}};
    int i = 0;
    for (const auto& p : gridVertices(n)) {
        v.rows.push_back({{p[0]}, {p[1]}, {p[2]}, {double((i * 7) % 256)}, {double((i * 13) % 256)}, {double((i * 29) % 256)}});
        ++i;
    }
    s.elements.push_back(v);
    s.elements.push_back(faceElem(gridFaces(n, quads)));
    writePly(ply, s);
}

std::vector<std::string> objLines(const fs::path& obj, const std::string& prefix = "") {
    std::ifstream in(obj.string(), std::ios::binary);
    std::vector<std::string> lines;
    std::string l;
    while (std::getline(in, l)) {
        if (l.empty() || l[0] == '#') continue;
        if (prefix.empty() || l.rfind(prefix, 0) == 0) lines.push_back(l);
    }
    return lines;
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p.string(), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct ObjStats {
    size_t vertices = 0, texcoords = 0, colors = 0, faces = 0;
    double sumX = 0, sumY = 0, sumZ = 0;
    std::vector<std::string> diffuseTextures;
};

ObjStats parseWithTinyObj(const fs::path& obj) {
    tinyobj::ObjReader reader;
    tinyobj::ObjReaderConfig config;
    config.triangulate = false;
    config.mtl_search_path = obj.parent_path().string();
    ObjStats s;
    EXPECT_TRUE(reader.ParseFromFile(obj.string(), config)) << reader.Error();
    const auto& a = reader.GetAttrib();
    s.vertices = a.vertices.size() / 3;
    s.texcoords = a.texcoords.size() / 2;
    s.colors = a.colors.size() / 3;
    for (size_t i = 0; i < s.vertices; ++i) {
        s.sumX += a.vertices[3 * i];
        s.sumY += a.vertices[3 * i + 1];
        s.sumZ += a.vertices[3 * i + 2];
    }
    for (const auto& sh : reader.GetShapes()) {
        for (auto fv : sh.mesh.num_face_vertices) {
            EXPECT_EQ(fv, 3u);
            ++s.faces;
        }
    }
    for (const auto& m : reader.GetMaterials()) s.diffuseTextures.push_back(m.diffuse_texname);
    return s;
}

// Independent (test-side) reader: sums x,y,z of a binary little-endian PLY whose
// first element is `vertex` with scalar properties only.
struct PlyPositions {
    uint64_t count = 0;
    double sumX = 0, sumY = 0, sumZ = 0;
};

bool sumPlyPositionsLE(const fs::path& ply, PlyPositions& out) {
    std::ifstream in(ply.string(), std::ios::binary);
    std::string line;
    std::vector<std::pair<std::string, std::string>> props;  // type, name
    bool inVertex = false, sawVertex = false, le = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream ss(line);
        std::string kw;
        ss >> kw;
        if (kw == "format") {
            std::string f;
            ss >> f;
            le = f == "binary_little_endian";
        } else if (kw == "element") {
            std::string name;
            uint64_t n;
            ss >> name >> n;
            if (!sawVertex && name != "vertex") return false;
            inVertex = name == "vertex" && !sawVertex;
            if (inVertex) out.count = n;
            sawVertex = true;
        } else if (kw == "property" && inVertex) {
            std::string t, name;
            ss >> t >> name;
            if (t == "list") return false;
            props.emplace_back(t, name);
        } else if (kw == "end_header") {
            break;
        }
    }
    if (!le || hostBigEndian()) return false;
    for (uint64_t i = 0; i < out.count; ++i) {
        for (const auto& p : props) {
            const auto& t = p.first;
            double v = 0;
            auto rd = [&](auto x) { in.read(reinterpret_cast<char*>(&x), sizeof(x)); v = static_cast<double>(x); };
            if (t == "float" || t == "float32") rd(float());
            else if (t == "double" || t == "float64") rd(double());
            else if (t == "uchar" || t == "uint8") rd(uint8_t());
            else if (t == "char" || t == "int8") rd(int8_t());
            else if (t == "ushort" || t == "uint16") rd(uint16_t());
            else if (t == "short" || t == "int16") rd(int16_t());
            else if (t == "uint" || t == "uint32") rd(uint32_t());
            else if (t == "int" || t == "int32") rd(int32_t());
            else return false;
            if (!in) return false;
            if (p.second == "x") out.sumX += v;
            else if (p.second == "y") out.sumY += v;
            else if (p.second == "z") out.sumZ += v;
        }
    }
    return true;
}

// Runs a conversion that must fail with an AppException whose message contains `needle`.
void expectConvertError(const fs::path& ply, const fs::path& outBase, const std::string& needle) {
    try {
        convertPlyToObj(ply.string(), outBase.string());
        FAIL() << "conversion of " << ply.filename() << " should fail with '" << needle << "'";
    } catch (const BuildDepMissingException& e) {
        FAIL() << "unexpected BuildDepMissingException: " << e.what();
    } catch (const AppException& e) {
        EXPECT_NE(std::string(e.what()).find(needle), std::string::npos) << e.what();
    }
    EXPECT_FALSE(fs::exists(outBase.string() + ".obj")) << "partial OBJ left behind";
    EXPECT_FALSE(fs::exists(outBase.string() + ".mtl")) << "partial MTL left behind";
}

// ---------------------------------------------------------------------------
// Header model / identifyPly / getPlyInfo
// ---------------------------------------------------------------------------

TEST(ply, headerModel) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("m.ply");
    PlySpec s;
    s.format = "binary_big_endian";
    s.comments = {"TextureFile  my tex.jpg  ", "hello"};
    s.elements.push_back(vertexElem(kSquare, "float32"));
    s.elements.push_back(faceElem(kSquareTris, "uint8", "uint32"));
    writePly(ply, s);

    PlyHeader h;
    std::string err;
    ASSERT_TRUE(parsePlyHeader(ply, h, &err)) << err;
    EXPECT_EQ(h.format, PlyFormat::BinaryBigEndian);
    EXPECT_EQ(h.version, "1.0");
    ASSERT_EQ(h.elements.size(), 2u);
    EXPECT_EQ(h.elements[0].name, "vertex");
    EXPECT_EQ(h.elements[0].count, 4u);
    EXPECT_EQ(h.elements[0].properties[0].type, PlyScalarType::Float32);
    EXPECT_EQ(h.elements[1].properties[0].isList, true);
    EXPECT_EQ(h.elements[1].properties[0].countType, PlyScalarType::UInt8);
    EXPECT_EQ(h.elements[1].properties[0].type, PlyScalarType::UInt32);
    EXPECT_TRUE(h.hasTextureFile);
    EXPECT_EQ(h.textureFilename, "my tex.jpg");
    ASSERT_EQ(h.comments.size(), 2u);

    // The body starts right after "end_header\n".
    const std::string all = readAll(ply);
    EXPECT_EQ(all.substr(0, h.headerLength).size(), h.headerLength);
    EXPECT_EQ(all.substr(h.headerLength - 11, 11), "end_header\n");
    EXPECT_EQ(all.size() - h.headerLength, 4u * 12u + 2u * (1u + 12u));

    // Every alias maps to a type.
    for (const char* t : {"char", "uchar", "short", "ushort", "int", "uint", "float", "double", "int8", "uint8",
                          "int16", "uint16", "int32", "uint32", "float32", "float64"})
        EXPECT_NE(plyScalarTypeFromName(t), PlyScalarType::Unknown) << t;
    EXPECT_EQ(plyScalarTypeFromName("int64"), PlyScalarType::Unknown);
}

TEST(ply, identifyFixtures) {
    TestArea ta(TEST_NAME);

    // F1: textured mesh -> Model (B4 spike PASS, see texturedPlyBuildsNexusF1); the
    // texture name is exposed.
    const fs::path f1 = ta.getPath("f1.ply");
    writeF1(f1);
    EXPECT_EQ(identifyPly(f1), EntryType::Model);
    PlyInfo i1;
    ASSERT_TRUE(getPlyInfo(f1, i1));
    EXPECT_TRUE(i1.isMesh);
    EXPECT_TRUE(i1.hasTextures);
    EXPECT_EQ(i1.textureFilename, "tex.jpg");
    EXPECT_EQ(i1.vertexCount, 36u);
    EXPECT_EQ(i1.dimensions, (std::vector<std::string>{"x", "y", "z", "vertex_indices", "texcoord"}));

    // F2: vertex s,t, no comment -> Model.
    const fs::path f2 = ta.getPath("f2.ply");
    writeF2(f2);
    EXPECT_EQ(identifyPly(f2), EntryType::Model);
    PlyInfo i2;
    ASSERT_TRUE(getPlyInfo(f2, i2));
    EXPECT_FALSE(i2.hasTextures);
    EXPECT_TRUE(i2.textureFilename.empty());

    // F3: untextured uchar rgb -> Model.
    const fs::path f3 = ta.getPath("f3.ply");
    writeF3(f3);
    EXPECT_EQ(identifyPly(f3), EntryType::Model);
}

TEST(ply, crlfHeader) {
    TestArea ta(TEST_NAME);
    const fs::path mesh = ta.getPath("crlf_mesh.ply");
    PlySpec s;
    s.crlf = true;
    s.elements.push_back(vertexElem(kSquare));
    s.elements.push_back(faceElem(kSquareTris));
    writePly(mesh, s);
    EXPECT_EQ(identifyPly(mesh), EntryType::Model);
    PlyInfo info;
    ASSERT_TRUE(getPlyInfo(mesh, info));
    EXPECT_EQ(info.vertexCount, 4u);
    EXPECT_EQ(info.dimensions, (std::vector<std::string>{"x", "y", "z", "vertex_indices"}));

    const fs::path cloud = ta.getPath("crlf_cloud.ply");
    PlySpec c;
    c.crlf = true;
    c.format = "ascii";
    c.elements.push_back(vertexElem(kSquare));
    writePly(cloud, c);
    EXPECT_EQ(identifyPly(cloud), EntryType::PointCloud);
}

// Keywords are matched as anchored tokens: text inside a comment never adds
// properties, elements or textures.
TEST(ply, commentTrap) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("trap.ply");
    PlySpec s;
    s.comments = {"this property float bogus is not a property", "element face 12", "see TextureFile x.jpg"};
    s.elements.push_back(vertexElem(kSquare));
    writePly(ply, s);

    PlyInfo info;
    ASSERT_TRUE(getPlyInfo(ply, info));
    EXPECT_EQ(info.dimensions, (std::vector<std::string>{"x", "y", "z"}));
    EXPECT_FALSE(info.isMesh);
    EXPECT_FALSE(info.hasTextures);
    EXPECT_EQ(identifyPly(ply), EntryType::PointCloud);
}

TEST(ply, malformedHeaders) {
    TestArea ta(TEST_NAME);
    const auto write = [&](const std::string& name, const std::string& body) {
        const fs::path p = ta.getPath(name);
        std::ofstream o(p.string(), std::ios::binary);
        o << body;
        return p;
    };
    PlyInfo info;
    EXPECT_FALSE(getPlyInfo(write("noply.ply", "plx\nformat ascii 1.0\nend_header\n"), info));
    EXPECT_FALSE(getPlyInfo(write("noend.ply", "ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\n"), info));
    EXPECT_FALSE(getPlyInfo(write("badcount.ply", "ply\nformat ascii 1.0\nelement vertex 1x\nend_header\n"), info));
    EXPECT_FALSE(getPlyInfo(write("orphan.ply", "ply\nformat ascii 1.0\nproperty float x\nend_header\n"), info));
    EXPECT_EQ(identifyPly(write("noend2.ply", "ply\nformat ascii 1.0\n")), EntryType::Generic);

    std::string longHeader = "ply\nformat ascii 1.0\nelement vertex 1\n";
    for (int i = 0; i < 600; ++i) longHeader += "property float p" + std::to_string(i) + "\n";
    longHeader += "end_header\n";
    EXPECT_FALSE(getPlyInfo(write("long.ply", longHeader), info));

    EXPECT_THROW(readPlyHeader(write("noend3.ply", "ply\n")), AppException);
}

// ---------------------------------------------------------------------------
// Converter: formats, types, polygons
// ---------------------------------------------------------------------------

TEST(plyconv, asciiTriangles) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("a.ply");
    PlySpec s;
    s.format = "ascii";
    s.elements.push_back(vertexElem(kSquare));
    s.elements.push_back(faceElem(kSquareTris));
    writePly(ply, s);

    const auto r = convertPlyToObj(ply.string(), ta.getPath("out/a").string());
    EXPECT_EQ(r.objPath, ta.getPath("out/a.obj").string());
    EXPECT_TRUE(r.mtlPath.empty());
    EXPECT_EQ(r.vertexCount, 4u);
    EXPECT_EQ(r.info.faceCount, 2u);
    ASSERT_TRUE(r.info.hasBounds);
    EXPECT_EQ(r.info.minX, 0.0);
    EXPECT_EQ(r.info.maxX, 1.0);
    EXPECT_EQ(r.info.maxY, 1.0);
    EXPECT_EQ(r.info.minZ, 0.0);
    EXPECT_EQ(r.info.maxZ, 0.5);

    EXPECT_EQ(objLines(r.objPath), (std::vector<std::string>{"v 0 0 0", "v 1 0 0", "v 1 1 0", "v 0 1 0.5",
                                                             "f 1 2 3", "f 1 3 4"}));
    const std::string all = readAll(r.objPath);
    EXPECT_EQ(all.find("mtllib"), std::string::npos);
    EXPECT_EQ(all.find("usemtl"), std::string::npos);
}

// The same mesh written as ASCII, binary LE and binary BE converts to identical OBJs.
TEST(plyconv, formatsProduceIdenticalObj) {
    TestArea ta(TEST_NAME);
    std::vector<std::string> outputs;
    for (const std::string fmt : {"ascii", "binary_little_endian", "binary_big_endian"}) {
        const fs::path ply = ta.getPath(fmt + ".ply");
        PlySpec s;
        s.format = fmt;
        Elem v{"vertex", {{"double", "x", ""}, {"float", "y", ""}, {"short", "z", ""}, {"uchar", "red", ""},
                          {"uchar", "green", ""}, {"uchar", "blue", ""}}, {}};
        v.rows.push_back({{0.1}, {0.2f}, {-3}, {255}, {0}, {51}});
        v.rows.push_back({{1.0 / 3.0}, {1.5f}, {7}, {1}, {2}, {3}});
        v.rows.push_back({{-2.25}, {1e-7f}, {0}, {10}, {20}, {30}});
        s.elements.push_back(v);
        s.elements.push_back(faceElem({{0, 1, 2}}, "ushort", "uint16"));
        writePly(ply, s);
        const auto r = convertPlyToObj(ply.string(), ta.getPath(fmt).string());
        EXPECT_EQ(r.info.faceCount, 1u);
        EXPECT_TRUE(r.hasColors);
        outputs.push_back(readAll(r.objPath).substr(readAll(r.objPath).find('\n') + 1));  // drop comment line
    }
    EXPECT_EQ(outputs[0], outputs[1]);
    EXPECT_EQ(outputs[1], outputs[2]);
    // %.17g for double sources, %.9g for float sources, integers exact, uchar / 255.
    EXPECT_NE(outputs[0].find("v 0.10000000000000001 0.200000003 -3 1 0 0.2\n"), std::string::npos) << outputs[0];
    EXPECT_NE(outputs[0].find("v 0.33333333333333331 1.5 7 "), std::string::npos) << outputs[0];
    EXPECT_NE(outputs[0].find("v -2.25 1.00000001e-07 0 "), std::string::npos) << outputs[0];
}

// Every scalar type and alias; list properties with any count/index type, skipped
// when unknown; unknown elements before the face element are skipped; elements after
// it are never read.
TEST(plyconv, allTypesListsAndUnknownElements) {
    TestArea ta(TEST_NAME);
    for (const std::string fmt : {"ascii", "binary_little_endian", "binary_big_endian"}) {
        const fs::path ply = ta.getPath("types_" + fmt + ".ply");
        PlySpec s;
        s.format = fmt;
        Elem v{"vertex",
               {{"int8", "a", ""}, {"float64", "x", ""}, {"uint8", "b", ""}, {"int16", "c", ""}, {"float32", "y", ""},
                {"uint16", "d", ""}, {"int32", "z", ""}, {"uint32", "e", ""}, {"char", "f", ""}, {"ushort", "g", ""},
                {"int", "junk", "ushort"}, {"uint", "h", ""}, {"double", "i", ""}},
               {}};
        for (int k = 0; k < 4; ++k)
            v.rows.push_back({{-1}, {kSquare[k][0]}, {200}, {-300}, {kSquare[k][1]}, {60000}, {static_cast<double>(k)}, {4000000000.0}, {-5},
                              {7}, {1, 2, 3}, {9}, {0.5}});
        s.elements.push_back(v);

        Elem material{"material", {{"float", "ambient", ""}, {"uchar", "name", "int"}}, {}};
        material.rows.push_back({{0.5}, {65, 66}});
        s.elements.push_back(material);

        Elem face{"face",
                  {{"short", "flags", ""}, {"uint16", "vertex_index", "int32"}, {"float", "other", "uint8"}, {"uchar", "tag", ""}},
                  {}};
        face.rows.push_back({{1}, {0, 1, 2, 3}, {0.1, 0.2}, {9}});
        s.elements.push_back(face);

        Elem edge{"edge", {{"int", "vertex1", ""}, {"int", "vertex2", ""}}, {}};
        edge.declaredCount = 100;  // truncated: must not matter, never read
        s.elements.push_back(edge);
        writePly(ply, s);

        const auto r = convertPlyToObj(ply.string(), ta.getPath("types_" + fmt).string());
        EXPECT_EQ(r.vertexCount, 4u) << fmt;
        EXPECT_EQ(r.info.faceCount, 2u) << fmt;
        EXPECT_FALSE(r.hasColors) << fmt;
        EXPECT_EQ(objLines(r.objPath), (std::vector<std::string>{"v 0 0 0", "v 1 0 1", "v 1 1 2", "v 0 1 3", "f 1 2 3",
                                                                 "f 1 3 4"}))
            << fmt;
    }
}

TEST(plyconv, crlfHeaderAndBody) {
    TestArea ta(TEST_NAME);
    for (const std::string fmt : {"ascii", "binary_little_endian"}) {
        const fs::path ply = ta.getPath("crlf_" + fmt + ".ply");
        PlySpec s;
        s.crlf = true;
        s.format = fmt;
        s.elements.push_back(vertexElem(kSquare));
        s.elements.push_back(faceElem(kSquareTris));
        writePly(ply, s);
        const auto r = convertPlyToObj(ply.string(), ta.getPath("crlf_" + fmt).string());
        EXPECT_EQ(objLines(r.objPath), (std::vector<std::string>{"v 0 0 0", "v 1 0 0", "v 1 1 0", "v 0 1 0.5",
                                                                 "f 1 2 3", "f 1 3 4"}))
            << fmt;
    }
}

TEST(plyconv, quadsAndNgons) {
    TestArea ta(TEST_NAME);
    const fs::path quad = ta.getPath("quad.ply");
    PlySpec q;
    q.elements.push_back(vertexElem(kSquare));
    q.elements.push_back(faceElem(kSquareQuad));
    writePly(quad, q);
    const auto rq = convertPlyToObj(quad.string(), ta.getPath("quad").string());
    EXPECT_EQ(rq.info.faceCount, 2u);
    EXPECT_EQ(objLines(rq.objPath, "f "), (std::vector<std::string>{"f 1 2 3", "f 1 3 4"}));

    const fs::path hex = ta.getPath("hex.ply");
    PlySpec h;
    h.format = "ascii";
    h.elements.push_back(vertexElem(hexagon()));
    // One hexagon, one degenerate 2-corner face (skipped) and one triangle.
    h.elements.push_back(faceElem({{0, 1, 2, 3, 4, 5}, {0, 1}, {5, 4, 3}}));
    writePly(hex, h);
    const auto rh = convertPlyToObj(hex.string(), ta.getPath("hex").string());
    EXPECT_EQ(rh.info.faceCount, 5u);
    EXPECT_EQ(objLines(rh.objPath, "f "),
              (std::vector<std::string>{"f 1 2 3", "f 1 3 4", "f 1 4 5", "f 1 5 6", "f 6 5 4"}));
    EXPECT_EQ(parseWithTinyObj(rh.objPath).faces, 5u);
}

// ---------------------------------------------------------------------------
// Colours and texture coordinates
// ---------------------------------------------------------------------------

TEST(plyconv, colorsAllOrNone) {
    TestArea ta(TEST_NAME);
    struct Case {
        std::string name;
        std::vector<Prop> colorProps;
        std::vector<double> values;
        bool expectColors;
        std::string expectedFirstV;
    };
    const std::vector<Case> cases = {
        {"uchar_rgb", {{"uchar", "red", ""}, {"uchar", "green", ""}, {"uchar", "blue", ""}}, {255, 0, 51}, true, "v 0 0 0 1 0 0.2"},
        {"float_diffuse", {{"float", "diffuse_red", ""}, {"float", "diffuse_green", ""}, {"float", "diffuse_blue", ""}},
         {0.25, 0.5, 1.0}, true, "v 0 0 0 0.25 0.5 1"},
        {"short_names_alpha", {{"uchar", "r", ""}, {"uchar", "g", ""}, {"uchar", "b", ""}, {"uchar", "a", ""}},
         {0, 255, 0, 17}, true, "v 0 0 0 0 1 0"},
        {"double_rgb", {{"double", "red", ""}, {"double", "green", ""}, {"double", "blue", ""}}, {0.1, 0.2, 0.3}, true,
         "v 0 0 0 0.10000000000000001 0.20000000000000001 0.29999999999999999"},
        {"partial", {{"uchar", "red", ""}, {"uchar", "green", ""}}, {1, 2}, false, "v 0 0 0"},
    };
    for (const auto& c : cases) {
        const fs::path ply = ta.getPath(c.name + ".ply");
        PlySpec s;
        Elem v = vertexElem(kSquare);
        for (const auto& p : c.colorProps) v.props.push_back(p);
        for (auto& row : v.rows)
            for (double x : c.values) row.push_back({x});
        s.elements.push_back(v);
        s.elements.push_back(faceElem(kSquareTris));
        writePly(ply, s);
        const auto r = convertPlyToObj(ply.string(), ta.getPath(c.name).string());
        EXPECT_EQ(r.hasColors, c.expectColors) << c.name;
        const auto vs = objLines(r.objPath, "v ");
        ASSERT_EQ(vs.size(), 4u) << c.name;
        EXPECT_EQ(vs[0], c.expectedFirstV) << c.name;
        const auto st = parseWithTinyObj(r.objPath);
        EXPECT_EQ(st.vertices, 4u);
    }
}

// A complete colour set of an unsupported type does not stop the search for a
// supported one; when it is the only complete set the conversion fails with the
// property and its type instead of silently dropping the colours.
TEST(plyconv, unsupportedColorTypes) {
    TestArea ta(TEST_NAME);
    const auto write = [&](const std::string& name, const std::vector<Prop>& props, const std::vector<double>& values) {
        const fs::path ply = ta.getPath(name + ".ply");
        PlySpec s;
        Elem v = vertexElem(kSquare);
        for (const auto& p : props) v.props.push_back(p);
        for (auto& row : v.rows)
            for (double x : values) row.push_back({x});
        s.elements.push_back(v);
        s.elements.push_back(faceElem(kSquareTris));
        writePly(ply, s);
        return ply;
    };

    // int red/green/blue + uchar diffuse_*: the diffuse set is used.
    const fs::path fallback = write("fallback",
                                    {{"int", "red", ""}, {"int", "green", ""}, {"int", "blue", ""},
                                     {"uchar", "diffuse_red", ""}, {"uchar", "diffuse_green", ""}, {"uchar", "diffuse_blue", ""}},
                                    {9, 9, 9, 255, 0, 51});
    const auto r = convertPlyToObj(fallback.string(), ta.getPath("fallback").string());
    EXPECT_TRUE(r.hasColors);
    const auto vs = objLines(r.objPath, "v ");
    ASSERT_EQ(vs.size(), 4u);
    EXPECT_EQ(vs[0], "v 0 0 0 1 0 0.2");

    // Only int red/green/blue: hard error naming the property and the type.
    const fs::path onlyInt = write("only_int", {{"uchar", "red", ""}, {"int", "green", ""}, {"uchar", "blue", ""}}, {1, 2, 3});
    expectConvertError(onlyInt, ta.getPath("only_int"), "vertex colour property 'green' has the unsupported type 'int'");
    EXPECT_FALSE(fs::exists(ta.getPath("only_int.obj")));

    // short r/g/b only (last candidate set).
    const fs::path onlyShort = write("only_short", {{"short", "r", ""}, {"short", "g", ""}, {"short", "b", ""}}, {1, 2, 3});
    expectConvertError(onlyShort, ta.getPath("only_short"), "vertex colour property 'r' has the unsupported type 'short'");
}

TEST(plyconv, vertexTexcoords) {
    TestArea ta(TEST_NAME);
    for (const auto& names : std::vector<std::pair<std::string, std::string>>{
             {"s", "t"}, {"u", "v"}, {"texture_u", "texture_v"}, {"texture_s", "texture_t"}}) {
        const fs::path ply = ta.getPath(names.first + ".ply");
        PlySpec s;
        Elem v = vertexElem(kSquare);
        v.props.push_back({"float", names.first, ""});
        v.props.push_back({"float", names.second, ""});
        for (size_t i = 0; i < v.rows.size(); ++i) {
            v.rows[i].push_back({kSquare[i][0]});
            v.rows[i].push_back({kSquare[i][1]});
        }
        s.elements.push_back(v);
        s.elements.push_back(faceElem(kSquareQuad));
        writePly(ply, s);
        const auto r = convertPlyToObj(ply.string(), ta.getPath(names.first).string());
        EXPECT_EQ(r.texcoordCount, 4u) << names.first;
        EXPECT_TRUE(r.mtlPath.empty());
        EXPECT_EQ(objLines(r.objPath, "vt "), (std::vector<std::string>{"vt 0 0", "vt 1 0", "vt 1 1", "vt 0 1"}));
        EXPECT_EQ(objLines(r.objPath, "f "), (std::vector<std::string>{"f 1/1 2/2 3/3", "f 1/1 3/3 4/4"}));
        const auto st = parseWithTinyObj(r.objPath);
        EXPECT_EQ(st.texcoords, 4u);
        EXPECT_EQ(st.faces, 2u);
    }
}

TEST(plyconv, perFaceTexcoords) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("ft.ply");
    PlySpec s;
    s.format = "ascii";
    s.elements.push_back(vertexElem(kSquare));
    Elem f{"face", {{"int", "vertex_indices", "uchar"}, {"float", "texcoord", "uchar"}}, {}};
    f.rows.push_back({{0, 1, 2, 3}, {0, 0, 0.5, 0, 0.5, 0.5, 0, 0.5}});
    f.rows.push_back({{3, 2, 1}, {0.1, 0.1, 0.2, 0.2, 0.3, 0.3}});
    s.elements.push_back(f);
    writePly(ply, s);

    const auto r = convertPlyToObj(ply.string(), ta.getPath("ft").string());
    EXPECT_EQ(r.texcoordCount, 7u);
    EXPECT_EQ(r.info.faceCount, 3u);
    const auto lines = objLines(r.objPath);
    const std::vector<std::string> expected = {
        "v 0 0 0", "v 1 0 0", "v 1 1 0", "v 0 1 0.5",
        "vt 0 0", "vt 0.5 0", "vt 0.5 0.5", "vt 0 0.5",
        "f 1/1 2/2 3/3", "f 1/1 3/3 4/4",
        "vt 0.100000001 0.100000001", "vt 0.200000003 0.200000003", "vt 0.300000012 0.300000012",
        "f 4/5 3/6 2/7"};
    EXPECT_EQ(lines, expected);
    const auto st = parseWithTinyObj(r.objPath);
    EXPECT_EQ(st.texcoords, 7u);
    EXPECT_EQ(st.faces, 3u);
}

// P9: the per-face texcoord list must hold 2 values per corner.
TEST(plyconv, perFaceTexcoordWrongCount) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("bad_ft.ply");
    PlySpec s;
    s.elements.push_back(vertexElem(kSquare));
    Elem f{"face", {{"int", "vertex_indices", "uchar"}, {"float", "texcoord", "uchar"}}, {}};
    f.rows.push_back({{0, 1, 2}, {0, 0, 1, 0, 1, 1}});
    f.rows.push_back({{0, 1, 2, 3}, {0, 0, 1, 0, 1, 1}});  // 6 values for 4 corners
    s.elements.push_back(f);
    writePly(ply, s);
    expectConvertError(ply, ta.getPath("bad_ft"), "face 1 has 6 texcoord values, expected 8");
}

// ---------------------------------------------------------------------------
// Unsupported / malformed input
// ---------------------------------------------------------------------------

TEST(plyconv, unsupportedInput) {
    TestArea ta(TEST_NAME);

    {  // face element before vertex element
        const fs::path p = ta.getPath("face_first.ply");
        PlySpec s;
        s.elements.push_back(faceElem(kSquareTris));
        s.elements.push_back(vertexElem(kSquare));
        writePly(p, s);
        expectConvertError(p, ta.getPath("face_first"), "precedes the 'vertex' element");
    }
    {  // missing vertex_indices
        const fs::path p = ta.getPath("no_indices.ply");
        PlySpec s;
        s.elements.push_back(vertexElem(kSquare));
        s.elements.push_back(faceElem(kSquareTris, "uchar", "int", "corners"));
        writePly(p, s);
        expectConvertError(p, ta.getPath("no_indices"), "lacks a 'vertex_indices'");
    }
    {  // bad format line
        const fs::path p = ta.getPath("bad_format.ply");
        PlySpec s;
        s.formatLine = "format binary_middle_endian 1.0";
        s.elements.push_back(vertexElem(kSquare));
        s.elements.push_back(faceElem(kSquareTris));
        writePly(p, s);
        expectConvertError(p, ta.getPath("bad_format"), "unsupported or missing format 'binary_middle_endian'");
    }
    {  // truncated binary body
        const fs::path p = ta.getPath("truncated.ply");
        PlySpec s;
        s.elements.push_back(vertexElem(kSquare));
        Elem f = faceElem(kSquareTris);
        f.declaredCount = 5;
        s.elements.push_back(f);
        writePly(p, s);
        expectConvertError(p, ta.getPath("truncated"), "unexpected end of file in element 'face' index 2");
    }
    {  // truncated vertex data
        const fs::path p = ta.getPath("truncated_v.ply");
        PlySpec s;
        Elem v = vertexElem(kSquare);
        v.declaredCount = 10;
        s.elements.push_back(v);
        s.elements.push_back(faceElem({}));
        writePly(p, s);
        expectConvertError(p, ta.getPath("truncated_v"), "element 'vertex' index");
    }
    {  // malformed ASCII value
        const fs::path p = ta.getPath("bad_ascii.ply");
        std::ofstream o(p.string(), std::ios::binary);
        o << "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\n"
             "element face 1\nproperty list uchar int vertex_indices\nend_header\n"
             "0 0 0\n1 0 0\n0 abc 0\n3 0 1 2\n";
        o.close();
        expectConvertError(p, ta.getPath("bad_ascii"), "malformed value 'abc' in element 'vertex' index 2 (property 'y')");
    }
    {  // index out of range
        const fs::path p = ta.getPath("oob.ply");
        PlySpec s;
        s.elements.push_back(vertexElem(kSquare));
        s.elements.push_back(faceElem({{0, 1, 2}, {0, 2, 3}, {1, 2, 4}}));
        writePly(p, s);
        expectConvertError(p, ta.getPath("oob"), "face 2 references vertex index 4 out of range (vertex count 4)");
    }
    {  // negative index
        const fs::path p = ta.getPath("neg.ply");
        PlySpec s;
        s.elements.push_back(vertexElem(kSquare));
        s.elements.push_back(faceElem({{0, -1, 2}}));
        writePly(p, s);
        expectConvertError(p, ta.getPath("neg"), "face 0 references vertex index -1 out of range");
    }
    {  // no faces at all (point cloud)
        const fs::path p = ta.getPath("cloud.ply");
        PlySpec s;
        s.elements.push_back(vertexElem(kSquare));
        writePly(p, s);
        expectConvertError(p, ta.getPath("cloud"), "no 'face' element");
    }
    {  // missing z
        const fs::path p = ta.getPath("noz.ply");
        PlySpec s;
        Elem v{"vertex", {{"float", "x", ""}, {"float", "y", ""}}, {}};
        for (const auto& q : kSquare) v.rows.push_back({{q[0]}, {q[1]}});
        s.elements.push_back(v);
        s.elements.push_back(faceElem(kSquareTris));
        writePly(p, s);
        expectConvertError(p, ta.getPath("noz"), "lacks scalar x, y and z");
    }
    {  // unknown property type
        const fs::path p = ta.getPath("int64.ply");
        PlySpec s;
        Elem v = vertexElem(kSquare);
        s.elements.push_back(v);
        s.elements.push_back(faceElem(kSquareTris));
        writePly(p, s);
        std::string body = readAll(p);
        body.replace(body.find("property float x"), 16, "property int64 x");
        std::ofstream(p.string(), std::ios::binary) << body;
        expectConvertError(p, ta.getPath("int64"), "unknown type 'int64'");
    }
    // missing file
    EXPECT_THROW(convertPlyToObj(ta.getPath("nope.ply").string(), ta.getPath("nope").string()), FSException);
}

// ---------------------------------------------------------------------------
// Texture (B4)
// ---------------------------------------------------------------------------

TEST(plyconv, textureWritten) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.getPath("ds/f1.ply");
    fs::create_directories(ply.parent_path());
    writeF1(ply);
    ASSERT_TRUE(writeJpeg(ta.getPath("ds/tex.jpg")));

    EXPECT_EQ(fs::path(getPlyTexture(ply.string(), ta.getPath("ds").string())), ta.getPath("ds/tex.jpg"));

    PlyToObjOptions opts;
    opts.datasetRoot = ta.getPath("ds").string();
    const auto r = convertPlyToObj(ply.string(), ta.getPath("out/model").string(), opts);
    ASSERT_FALSE(r.mtlPath.empty());
    EXPECT_EQ(r.mtlPath, ta.getPath("out/model.mtl").string());
    EXPECT_TRUE(fs::exists(ta.getPath("out/tex.jpg")));
    EXPECT_EQ(r.texturePath, ta.getPath("out/tex.jpg").string());
    EXPECT_EQ(fs::file_size(ta.getPath("out/tex.jpg")), fs::file_size(ta.getPath("ds/tex.jpg")));

    const auto lines = objLines(r.objPath);
    ASSERT_GE(lines.size(), 2u);
    EXPECT_EQ(lines[0], "mtllib model.mtl");
    EXPECT_EQ(lines[1], "usemtl material0");
    EXPECT_EQ(objLines(r.mtlPath), (std::vector<std::string>{"newmtl material0", "map_Kd tex.jpg"}));

    const auto st = parseWithTinyObj(r.objPath);
    EXPECT_EQ(st.faces, 50u);
    EXPECT_EQ(st.texcoords, 150u);
    ASSERT_EQ(st.diffuseTextures.size(), 1u);
    EXPECT_EQ(st.diffuseTextures[0], "tex.jpg");

    // withTexture = false: the comment is ignored, texcoords are kept.
    PlyToObjOptions noTex;
    noTex.withTexture = false;
    const auto r2 = convertPlyToObj(ply.string(), ta.getPath("out2/model").string(), noTex);
    EXPECT_TRUE(r2.mtlPath.empty());
    EXPECT_EQ(r2.texcoordCount, 150u);
    EXPECT_EQ(readAll(r2.objPath).find("mtllib"), std::string::npos);
    EXPECT_FALSE(fs::exists(ta.getPath("out2/tex.jpg")));

    // A name with spaces is copied under a whitespace-free name.
    const fs::path spaced = ta.getPath("ds/spaced.ply");
    writeF1(spaced, "TextureFile my tex.jpg");
    fs::copy_file(ta.getPath("ds/tex.jpg"), ta.getPath("ds/my tex.jpg"), fs::copy_options::overwrite_existing);
    const auto r3 = convertPlyToObj(spaced.string(), ta.getPath("out3/m").string());
    EXPECT_EQ(objLines(r3.mtlPath), (std::vector<std::string>{"newmtl material0", "map_Kd my_tex.jpg"}));
    EXPECT_TRUE(fs::exists(ta.getPath("out3/my_tex.jpg")));
}

// MeshLab-style multi-texture meshes (several TextureFile comments and/or a
// per-face texnumber) cannot be converted faithfully: identifyPly keeps them
// Generic and the textured conversion fails with a clear reason instead of
// silently mapping every face onto the first texture.
TEST(plyconv, multiTextureRejected) {
    TestArea ta(TEST_NAME);
    ASSERT_TRUE(writeJpeg(ta.getPath("t0.jpg")));
    ASSERT_TRUE(writeJpeg(ta.getPath("t1.jpg")));

    const auto writeMesh = [&](const fs::path& ply, const std::vector<std::string>& comments, bool texnumber) {
        PlySpec s;
        s.comments = comments;
        s.elements.push_back(vertexElem(kSquare));
        Elem f{"face", {{"int", "vertex_indices", "uchar"}, {"float", "texcoord", "uchar"}}, {}};
        if (texnumber) f.props.push_back({"int", "texnumber", ""});
        int i = 0;
        for (const auto& face : kSquareTris) {
            std::vector<double> tc;
            for (int idx : face) {
                tc.push_back(kSquare[idx][0]);
                tc.push_back(kSquare[idx][1]);
            }
            Row row{std::vector<double>(face.begin(), face.end()), tc};
            if (texnumber) row.push_back({double(i % 2)});
            f.rows.push_back(row);
            ++i;
        }
        s.elements.push_back(f);
        writePly(ply, s);
    };

    struct Case {
        std::string name;
        std::vector<std::string> comments;
        bool texnumber;
        std::string needle;
    };
    const std::vector<Case> cases = {
        {"two_textures", {"TextureFile t0.jpg", "TextureFile t1.jpg"}, true, "2 TextureFile comments"},
        {"two_textures_no_texnumber", {"TextureFile t0.jpg", "TextureFile t1.jpg"}, false, "2 TextureFile comments"},
        {"texnumber_one_texture", {"TextureFile t0.jpg"}, true, "'texnumber'"},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        const fs::path ply = ta.getPath(c.name + ".ply");
        writeMesh(ply, c.comments, c.texnumber);

        EXPECT_EQ(identifyPly(ply), EntryType::Generic);
        PlyInfo info;
        ASSERT_TRUE(getPlyInfo(ply, info));
        EXPECT_TRUE(info.isMesh);
        EXPECT_EQ(info.textureFileCount, c.comments.size());
        EXPECT_NE(info.unsupportedTextureReason.find(c.needle), std::string::npos) << info.unsupportedTextureReason;

        PlyHeader h = readPlyHeader(ply);
        EXPECT_EQ(h.textureFilenames.size(), c.comments.size());
        EXPECT_EQ(h.textureFilename, "t0.jpg");

        expectConvertError(ply, ta.getPath(c.name), c.needle);
        EXPECT_FALSE(fs::exists(ta.getPath(c.name + ".obj")));
        EXPECT_FALSE(fs::exists(ta.getPath(c.name + ".mtl")));
        try {
            getPlyTexture(ply.string());
            FAIL() << "getPlyTexture should reject a multi-texture mesh";
        } catch (const BuildDepMissingException& e) {
            FAIL() << "unexpected BuildDepMissingException: " << e.what();
        } catch (const AppException& e) {
            EXPECT_NE(std::string(e.what()).find(c.needle), std::string::npos) << e.what();
        }

        // Without the texture the geometry still converts.
        PlyToObjOptions noTex;
        noTex.withTexture = false;
        const auto r = convertPlyToObj(ply.string(), ta.getPath(c.name + "_notex").string(), noTex);
        EXPECT_TRUE(r.mtlPath.empty());
        EXPECT_EQ(r.info.faceCount, 2u);
    }

    // A texnumber column without any TextureFile is an untextured mesh, not a multi-texture one
    const fs::path untextured = ta.getPath("texnumber_no_texture.ply");
    writeMesh(untextured, {}, true);
    EXPECT_EQ(identifyPly(untextured), EntryType::Model);
    PlyInfo untexturedInfo;
    ASSERT_TRUE(getPlyInfo(untextured, untexturedInfo));
    EXPECT_TRUE(untexturedInfo.unsupportedTextureReason.empty()) << untexturedInfo.unsupportedTextureReason;
    EXPECT_EQ(getPlyTexture(untextured.string()), "");
    const auto untexturedRes = convertPlyToObj(untextured.string(), ta.getPath("texnumber_no_texture").string());
    EXPECT_TRUE(untexturedRes.mtlPath.empty());
    EXPECT_EQ(untexturedRes.info.faceCount, 2u);

    // Single texture, no texnumber: unchanged (Model, converted with its texture).
    const fs::path single = ta.getPath("single.ply");
    writeMesh(single, {"TextureFile t0.jpg"}, false);
    EXPECT_EQ(identifyPly(single), EntryType::Model);
    PlyInfo info;
    ASSERT_TRUE(getPlyInfo(single, info));
    EXPECT_EQ(info.textureFileCount, 1u);
    EXPECT_TRUE(info.unsupportedTextureReason.empty());
    const auto r = convertPlyToObj(single.string(), ta.getPath("single_out/m").string());
    EXPECT_FALSE(r.mtlPath.empty());
}

TEST(plyconv, textureDependencyContract) {
    TestArea ta(TEST_NAME);
    const fs::path root = ta.getFolder("ds");
    fs::create_directories(root / "sub");

    // Missing texture -> BuildDepMissingException naming it relative to the PLY folder,
    // thrown before any output is written.
    const fs::path f1 = root / "sub" / "f1.ply";
    writeF1(f1, "TextureFile .\\textures\\tex.jpg");
    for (int pass = 0; pass < 2; ++pass) {
        try {
            if (pass == 0)
                getPlyTexture(f1.string(), root.string());
            else
                convertPlyToObj(f1.string(), ta.getPath("out/m").string());
            FAIL() << "expected BuildDepMissingException";
        } catch (const BuildDepMissingException& e) {
            EXPECT_EQ(e.getMissingDependencies(), (std::vector<std::string>{"textures/tex.jpg"}));
        }
    }
    EXPECT_FALSE(fs::exists(ta.getPath("out/m.obj")));

    // Absolute names are unsatisfiable: AppException, never BuildDepMissingException.
    for (const std::string abs : {"/abs/tex.jpg", "C:\\tex.jpg", "\\\\server\\share\\tex.jpg"}) {
        const fs::path p = root / ("abs" + std::to_string(abs.size()) + ".ply");
        writeF1(p, "TextureFile " + abs);
        try {
            getPlyTexture(p.string());
            FAIL() << "expected AppException for " << abs;
        } catch (const BuildDepMissingException&) {
            FAIL() << "absolute texture reported as missing dependency: " << abs;
        } catch (const AppException& e) {
            EXPECT_NE(std::string(e.what()).find("absolute"), std::string::npos) << e.what();
        }
    }

    // Root-escaping name: unsatisfiable when the dataset root is known.
    const fs::path esc = root / "esc.ply";
    writeF1(esc, "TextureFile ../outside.jpg");
    ASSERT_TRUE(writeJpeg(ta.getPath("outside.jpg")));
    try {
        getPlyTexture(esc.string(), root.string());
        FAIL() << "expected AppException";
    } catch (const BuildDepMissingException&) {
        FAIL() << "root-escaping texture reported as missing dependency";
    } catch (const AppException& e) {
        EXPECT_NE(std::string(e.what()).find("outside the dataset"), std::string::npos) << e.what();
    }
    // Without a known root the same file resolves.
    EXPECT_FALSE(getPlyTexture(esc.string()).empty());

    // "../" staying inside the dataset is allowed.
    const fs::path inside = root / "sub" / "inside.ply";
    writeF1(inside, "TextureFile ../shared.jpg");
    ASSERT_TRUE(writeJpeg(root / "shared.jpg"));
    PlyToObjOptions opts;
    opts.datasetRoot = root.string();
    const auto r = convertPlyToObj(inside.string(), ta.getPath("out/inside").string(), opts);
    EXPECT_TRUE(fs::exists(ta.getPath("out/shared.jpg")));
    EXPECT_FALSE(r.mtlPath.empty());

    // No TextureFile -> empty.
    const fs::path f3 = root / "f3.ply";
    writeF3(f3);
    EXPECT_TRUE(getPlyTexture(f3.string(), root.string()).empty());
}

// B4 spike (fixture F1), kept as the regression test: the converter's OBJ + MTL +
// texture for a textured PLY is accepted by buildNexus and yields a non-empty .nxz
// that carries the texture (larger than the same mesh converted without it).
TEST(plyconv, texturedPlyBuildsNexusF1) {
    TestArea ta(TEST_NAME);
    const fs::path root = ta.getFolder("ds");
    const fs::path ply = root / "f1.ply";
    writeF1(ply);
    ASSERT_TRUE(writeJpeg(root / "tex.jpg", 64, 64));

    PlyToObjOptions opts;
    opts.datasetRoot = root.string();
    const auto r = convertPlyToObj(ply.string(), ta.getPath("out/f1").string(), opts);
    ASSERT_FALSE(r.mtlPath.empty());
    ASSERT_TRUE(fs::exists(r.texturePath));

    const std::string nxz = buildNexus(r.objPath, ta.getPath("out/f1.nxz").string(), true);
    ASSERT_TRUE(fs::exists(nxz));
    const auto texturedSize = fs::file_size(nxz);
    EXPECT_GT(texturedSize, 0u);

    PlyToObjOptions noTex;
    noTex.withTexture = false;
    const auto r2 = convertPlyToObj(ply.string(), ta.getPath("out2/f1").string(), noTex);
    const std::string nxz2 = buildNexus(r2.objPath, ta.getPath("out2/f1.nxz").string(), true);
    ASSERT_TRUE(fs::exists(nxz2));
    EXPECT_GT(texturedSize, fs::file_size(nxz2));
}

// B4 through build(): a textured PLY is indexed as a Model, its texture is a
// dependency (header-only check inside buildNexus, before Nexus runs).
struct PlyDataset {
    explicit PlyDataset(const std::string& name) : ta(name, true) {
        root = ta.getFolder("ds");
        ddb::initIndex(root.string());
        db = ddb::open(root.string(), true);
    }
    void add(const std::string& rel) { ddb::addToIndex(db.get(), {(root / rel).string()}); }
    Entry entry(const std::string& rel) {
        Entry e;
        EXPECT_TRUE(ddb::getEntry(db.get(), rel, e)) << rel;
        return e;
    }
    fs::path out(const std::string& rel) { return db->buildDirectory() / entry(rel).hash; }

    TestArea ta;
    fs::path root;
    std::unique_ptr<Database> db;
};

TEST(plyconv, texturedPlyBuildDefersUntilTextureExists) {
    PlyDataset ds(TEST_NAME);
    fs::create_directories(ds.root / "sub");
    writeF1(ds.root / "sub" / "f1.ply");
    ds.add("sub/f1.ply");
    ASSERT_EQ(ds.entry("sub/f1.ply").type, EntryType::Model);

    EXPECT_THROW(ddb::build(ds.db.get(), "sub/f1.ply", "", false), BuildDepMissingException);
    const auto pending = ddb::getPendingBuildInfo(ds.db.get());
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending[0].missingDependencies, (std::vector<std::string>{"sub/tex.jpg"}));
    EXPECT_FALSE(fs::exists(ds.out("sub/f1.ply") / "nxs"));

    ASSERT_TRUE(writeJpeg(ds.root / "sub" / "tex.jpg", 32, 32));
    ds.add("sub/tex.jpg");
    ddb::build(ds.db.get(), "sub/f1.ply", "", false);

    EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
    EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "sub/f1.ply"));
    const fs::path nxz = ds.out("sub/f1.ply") / "nxs" / "model.nxz";
    ASSERT_TRUE(fs::exists(nxz));
    EXPECT_GT(fs::file_size(nxz), 0u);
    if (!obj2tiles::findObj2TilesBinary(true).empty())
        EXPECT_TRUE(fs::exists(ds.out("sub/f1.ply") / "3dtiles" / "tileset.json"));
}

TEST(plyconv, texturedPlyUnsatisfiableTextureFailsWithoutPending) {
    PlyDataset ds(TEST_NAME);
    writeF1(ds.root / "abs.ply", "TextureFile /abs/tex.jpg");
    writeF1(ds.root / "esc.ply", "TextureFile ../outside.jpg");
    ASSERT_TRUE(writeJpeg(ds.ta.getPath("outside.jpg")));  // next to the dataset root
    ds.add("abs.ply");
    ds.add("esc.ply");

    for (const std::string rel : {"abs.ply", "esc.ply"}) {
        ASSERT_EQ(ds.entry(rel).type, EntryType::Model);
        try {
            ddb::build(ds.db.get(), rel, "", false);
            FAIL() << rel << " should fail";
        } catch (const BuildDepMissingException& e) {
            FAIL() << "unsatisfiable texture must not defer the build: " << e.what();
        } catch (const AppException& e) {
            SUCCEED() << e.what();
        }
    }
    EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
}

// ---------------------------------------------------------------------------
// Differential test against Assimp (ddb::getModelInfo) and tinyobjloader
// ---------------------------------------------------------------------------

// Assimp (getModelInfo) gives face count and bounds of the PLY and of the produced
// OBJ; tinyobjloader gives the OBJ's vertex count and positions; a test-side PLY
// reader gives the reference position checksum. getModelInfo does not expose a
// vertex count (and Assimp's OBJ importer re-indexes vertices per face corner),
// so the vertex count is compared with the PLY header instead.
void differential(const fs::path& ply, const fs::path& outBase) {
    SCOPED_TRACE(ply.filename().string());
    PlyInfo pinfo;
    ASSERT_TRUE(getPlyInfo(ply, pinfo));

    PlyToObjOptions opts;
    opts.withTexture = false;  // geometry only (Assimp does not need the texture)
    const auto r = convertPlyToObj(ply.string(), outBase.string(), opts);

    ModelInfo aPly, aObj;
    ASSERT_TRUE(getModelInfo(ply.string(), aPly));
    ASSERT_TRUE(getModelInfo(r.objPath, aObj));

    // Face count: converter == Assimp(PLY) == Assimp(OBJ) == tinyobj.
    EXPECT_EQ(r.info.faceCount, aPly.faceCount);
    EXPECT_EQ(aObj.faceCount, aPly.faceCount);

    const auto st = parseWithTinyObj(r.objPath);
    EXPECT_EQ(st.faces, r.info.faceCount);

    // Vertex count: header == converter == tinyobj.
    EXPECT_EQ(r.vertexCount, pinfo.vertexCount);
    EXPECT_EQ(st.vertices, pinfo.vertexCount);

    // Bounds: Assimp works in float; outputs round-trip float values exactly, so
    // allow only float rounding on the extent.
    const double extent = std::max({aPly.maxX - aPly.minX, aPly.maxY - aPly.minY, aPly.maxZ - aPly.minZ, 1.0});
    const double tol = 1e-6 * extent;
    for (const auto* a : {&aPly, &aObj}) {
        EXPECT_NEAR(r.info.minX, a->minX, tol);
        EXPECT_NEAR(r.info.minY, a->minY, tol);
        EXPECT_NEAR(r.info.minZ, a->minZ, tol);
        EXPECT_NEAR(r.info.maxX, a->maxX, tol);
        EXPECT_NEAR(r.info.maxY, a->maxY, tol);
        EXPECT_NEAR(r.info.maxZ, a->maxZ, tol);
    }

    // Position checksum against an independent reader of the PLY body.
    PlyPositions ref;
    ASSERT_TRUE(sumPlyPositionsLE(ply, ref));
    EXPECT_EQ(ref.count, st.vertices);
    const auto close = [](double a, double b) { return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::fabs(b)); };
    EXPECT_TRUE(close(st.sumX, ref.sumX)) << st.sumX << " vs " << ref.sumX;
    EXPECT_TRUE(close(st.sumY, ref.sumY)) << st.sumY << " vs " << ref.sumY;
    EXPECT_TRUE(close(st.sumZ, ref.sumZ)) << st.sumZ << " vs " << ref.sumZ;
}

TEST(plyconv, differentialFixtures) {
    TestArea ta(TEST_NAME);
    writeF1(ta.getPath("f1.ply"));
    ASSERT_TRUE(writeJpeg(ta.getPath("tex.jpg")));
    writeF2(ta.getPath("f2.ply"));
    writeF3(ta.getPath("f3.ply"));
    writeF3(ta.getPath("f3q.ply"), 9, true);
    for (const std::string n : {"f1", "f2", "f3", "f3q"})
        differential(ta.getPath(n + ".ply"), ta.getPath("out/" + n));
}

TEST(plyconv, differentialBrighton) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/refs/heads/master/3d/brighton.ply", "brighton.ply");
    differential(ply, ta.getPath("out/brighton"));
    PlyInfo info;
    ASSERT_TRUE(getPlyInfo(ply, info));
    EXPECT_EQ(info.vertexCount, 386557u);
}

TEST(plyconv, differentialOdm25dMesh) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/refs/heads/master/point-clouds/odm_25dmesh.ply", "odm_25dmesh.ply");
    differential(ply, ta.getPath("out/odm_25dmesh"));
}

// Convert-only run of brighton.ply, for timing / peak-RSS measurements:
//   /usr/bin/time -v ./ddbtest --gtest_also_run_disabled_tests --gtest_filter='plyconv.DISABLED_convertBrighton'
MANUAL_TEST(plyconv, convertBrighton) {
    TestArea ta(TEST_NAME);
    const fs::path ply = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/refs/heads/master/3d/brighton.ply", "brighton.ply");
    const auto r = convertPlyToObj(ply.string(), ta.getPath("out/brighton").string());
    EXPECT_EQ(r.info.faceCount, 772263u);
    EXPECT_TRUE(r.hasColors);
}

// ---------------------------------------------------------------------------
// The produced OBJ parses in Obj2Tiles (skipped when the binary is absent)
// ---------------------------------------------------------------------------

void runThroughObj2Tiles(const fs::path& obj, const fs::path& out) {
    obj2tiles::Obj2TilesOptions opts;
    opts.lods = 2;
    opts.divisions = 1;
    std::string err;
    ASSERT_TRUE(obj2tiles::runObj2Tiles(obj, out, opts, err)) << err;
    EXPECT_TRUE(fs::exists(out / "tileset.json"));
}

TEST(plyconv, obj2tilesParsesOutput) {
    if (obj2tiles::findObj2TilesBinary(true).empty())
        GTEST_SKIP() << "Obj2Tiles binary not available";

    TestArea ta(TEST_NAME);
    writeF3(ta.getPath("f3.ply"), 12);
    const auto r3 = convertPlyToObj(ta.getPath("f3.ply").string(), ta.getPath("f3").string());
    runThroughObj2Tiles(r3.objPath, ta.getPath("tiles_f3"));

    // Quads + n-gons with vertex colours.
    PlySpec s;
    s.format = "ascii";
    std::vector<Vec3> v = gridVertices(10);
    const auto hex = hexagon();
    const int base = static_cast<int>(v.size());
    for (const auto& h : hex) v.push_back({h[0] + 10, h[1] + 10, h[2]});
    Elem ve{"vertex", {{"float", "x", ""}, {"float", "y", ""}, {"float", "z", ""}, {"float", "red", ""}, {"float", "green", ""}, {"float", "blue", ""}}, {}};
    for (const auto& p : v) ve.rows.push_back({{p[0]}, {p[1]}, {p[2]}, {0.2}, {0.4}, {0.6}});
    s.elements.push_back(ve);
    auto faces = gridFaces(10, true);
    faces.push_back({base, base + 1, base + 2, base + 3, base + 4, base + 5});
    s.elements.push_back(faceElem(faces));
    writePly(ta.getPath("ngon.ply"), s);
    const auto rn = convertPlyToObj(ta.getPath("ngon.ply").string(), ta.getPath("ngon").string());
    EXPECT_EQ(rn.info.faceCount, 81u * 2u + 4u);
    runThroughObj2Tiles(rn.objPath, ta.getPath("tiles_ngon"));
}

}  // namespace
