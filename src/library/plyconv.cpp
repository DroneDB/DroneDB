/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "plyconv.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <locale>
#include <sstream>
#include <system_error>
#include <vector>

#include "deppath.h"
#include "exceptions.h"
#include "logger.h"

namespace ddb
{

    namespace
    {
        // Largest polygon accepted (corners). Guards the per-face corner buffer against
        // a corrupt list count; real meshes use triangles, quads or small n-gons.
        constexpr uint64_t kMaxFaceCorners = 1u << 20;

        // I/O chunk size for both the PLY body reader and the OBJ writer.
        constexpr size_t kIoChunk = 1u << 20;

        bool hostIsBigEndian()
        {
            const uint16_t one = 1;
            unsigned char b;
            std::memcpy(&b, &one, 1);
            return b == 0;
        }

        // ------------------------------------------------------------------
        // Locale-independent number formatting / parsing
        // ------------------------------------------------------------------

        void appendNumber(std::string &out, double v, int precision)
        {
            char buf[64];
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
            const auto r = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::general, precision);
            if (r.ec != std::errc())
                throw AppException("Cannot format an OBJ numeric value");
            out.append(buf, r.ptr);
#else
            const int n = std::snprintf(buf, sizeof(buf), "%.*g", precision, v);
            if (n <= 0 || static_cast<size_t>(n) >= sizeof(buf))
                throw AppException("Cannot format an OBJ numeric value");
            // %g never groups thousands; only the decimal separator can be localised.
            const char *dp = std::localeconv()->decimal_point;
            const char sep = (dp != nullptr && dp[0] != '\0') ? dp[0] : '.';
            for (int i = 0; i < n; ++i)
                if (buf[i] == sep)
                    buf[i] = '.';
            out.append(buf, static_cast<size_t>(n));
#endif
        }

        void appendUInt(std::string &out, uint64_t v)
        {
            char buf[24];
            const auto r = std::to_chars(buf, buf + sizeof(buf), v);
            if (r.ec != std::errc())
                throw AppException("Cannot format an OBJ index");
            out.append(buf, r.ptr);
        }

        // Precision that round-trips a value read from a property of type t:
        // float sources print with 9 significant digits, everything else with 17
        // (integers print exactly either way).
        int precisionFor(PlyScalarType t)
        {
            return t == PlyScalarType::Float32 ? 9 : 17;
        }

        bool parseAsciiNumber(const std::string &tok, double &v)
        {
            if (tok.empty())
                return false;
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
            const char *first = tok.data();
            const char *last = tok.data() + tok.size();
            if (*first == '+')
                ++first; // from_chars rejects a leading '+'
            const auto r = std::from_chars(first, last, v, std::chars_format::general);
            return r.ec == std::errc() && r.ptr == last;
#else
            std::istringstream ss(tok);
            ss.imbue(std::locale::classic());
            ss >> v;
            return !ss.fail() && ss.peek() == std::char_traits<char>::eof();
#endif
        }

        std::string typeNameOf(const PlyProperty &p)
        {
            return p.isList ? ("list " + p.countTypeName + " " + p.typeName) : p.typeName;
        }

        // ------------------------------------------------------------------
        // Buffered PLY body reader (binary or ASCII)
        // ------------------------------------------------------------------

        enum class ReadStatus
        {
            Ok,
            Eof,
            Malformed
        };

        class BodyReader
        {
        public:
            BodyReader(const fs::path &file, uint64_t offset, PlyFormat format)
                : in(file.string(), std::ios::binary), ascii(format == PlyFormat::Ascii),
                  swap((format == PlyFormat::BinaryBigEndian) != hostIsBigEndian()),
                  buf(kIoChunk)
            {
                if (!in.is_open())
                    throw FSException("Cannot open " + file.string());
                in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
                if (!in)
                    throw FSException("Cannot seek to the body of " + file.string());
            }

            // Reads one value of type t into v (as a double: exact for every PLY
            // integer type and for float/double).
            ReadStatus read(PlyScalarType t, double &v)
            {
                if (ascii)
                {
                    if (!nextToken(token))
                        return ReadStatus::Eof;
                    if (!parseAsciiNumber(token, v))
                        return ReadStatus::Malformed;
                    // A float property holds a float: round like the binary encodings do,
                    // so ASCII and binary inputs convert to the same OBJ.
                    if (t == PlyScalarType::Float32 && std::isfinite(v))
                        v = static_cast<double>(static_cast<float>(v));
                    return ReadStatus::Ok;
                }

                const size_t n = plyScalarSize(t);
                unsigned char b[8];
                if (!readBytes(b, n))
                    return ReadStatus::Eof;
                if (swap)
                    std::reverse(b, b + n);
                switch (t)
                {
                    case PlyScalarType::Int8: { int8_t x; std::memcpy(&x, b, 1); v = x; break; }
                    case PlyScalarType::UInt8: { uint8_t x; std::memcpy(&x, b, 1); v = x; break; }
                    case PlyScalarType::Int16: { int16_t x; std::memcpy(&x, b, 2); v = x; break; }
                    case PlyScalarType::UInt16: { uint16_t x; std::memcpy(&x, b, 2); v = x; break; }
                    case PlyScalarType::Int32: { int32_t x; std::memcpy(&x, b, 4); v = x; break; }
                    case PlyScalarType::UInt32: { uint32_t x; std::memcpy(&x, b, 4); v = x; break; }
                    case PlyScalarType::Float32: { float x; std::memcpy(&x, b, 4); v = x; break; }
                    case PlyScalarType::Float64: { double x; std::memcpy(&x, b, 8); v = x; break; }
                    default: return ReadStatus::Malformed;
                }
                return ReadStatus::Ok;
            }

            // Last ASCII token read (for error messages).
            const std::string &lastToken() const { return token; }

        private:
            bool fill()
            {
                if (eof)
                    return false;
                in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
                len = static_cast<size_t>(in.gcount());
                pos = 0;
                if (len == 0)
                {
                    eof = true;
                    return false;
                }
                return true;
            }

            bool readBytes(unsigned char *dst, size_t n)
            {
                while (n > 0)
                {
                    if (pos == len && !fill())
                        return false;
                    const size_t k = std::min(n, len - pos);
                    std::memcpy(dst, buf.data() + pos, k);
                    pos += k;
                    dst += k;
                    n -= k;
                }
                return true;
            }

            static bool isSpace(char c)
            {
                return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
            }

            bool nextToken(std::string &out)
            {
                out.clear();
                // Skip whitespace (including line breaks: ASCII PLY values are a
                // whitespace-separated stream).
                while (true)
                {
                    if (pos == len && !fill())
                        return false;
                    if (!isSpace(buf[pos]))
                        break;
                    ++pos;
                }
                while (true)
                {
                    if (pos == len && !fill())
                        break;
                    const char c = buf[pos];
                    if (isSpace(c))
                        break;
                    if (out.size() >= 256)
                        break; // absurdly long token: returned as-is, it will not parse
                    out.push_back(c);
                    ++pos;
                }
                return true;
            }

            std::ifstream in;
            bool ascii;
            bool swap;
            std::vector<char> buf;
            size_t pos = 0;
            size_t len = 0;
            bool eof = false;
            std::string token;
        };

        // ------------------------------------------------------------------
        // Buffered OBJ/MTL writer
        // ------------------------------------------------------------------

        class TextWriter
        {
        public:
            explicit TextWriter(const fs::path &p) : path(p)
            {
                errno = 0;
                out.open(p.string(), std::ios::binary | std::ios::trunc);
                const int systemError = errno;
                if (!out.is_open())
                    throw FSException("Cannot write " + p.string(),
                                      std::error_code(systemError, std::generic_category()));
                data.reserve(kIoChunk + 4096);
            }

            std::string &buffer() { return data; }

            void maybeFlush()
            {
                if (data.size() >= kIoChunk)
                    flush();
            }

            void flush()
            {
                if (!data.empty())
                {
                    errno = 0;
                    out.write(data.data(), static_cast<std::streamsize>(data.size()));
                    const int systemError = errno;
                    if (!out)
                        throw FSException("Cannot write " + path.string(),
                                          std::error_code(systemError, std::generic_category()));
                    data.clear();
                }
                errno = 0;
                out.flush();
                const int systemError = errno;
                if (!out)
                    throw FSException("Cannot flush " + path.string(),
                                      std::error_code(systemError, std::generic_category()));
            }

            void close()
            {
                flush();
                errno = 0;
                out.close();
                const int systemError = errno;
                if (out.fail())
                    throw FSException("Cannot close " + path.string(),
                                      std::error_code(systemError, std::generic_category()));
            }

        private:
            fs::path path;
            std::ofstream out;
            std::string data;
        };

        // Removes the listed files unless dismissed (partial outputs on failure).
        class OutputGuard
        {
        public:
            void add(const fs::path &p) { files.push_back(p); }
            void dismiss() { files.clear(); }
            ~OutputGuard()
            {
                for (const auto &f : files)
                {
                    std::error_code ec;
                    fs::remove(f, ec);
                }
            }

        private:
            std::vector<fs::path> files;
        };

        // ------------------------------------------------------------------
        // Texture dependency
        // ------------------------------------------------------------------

        // Resolves the TextureFile of a PLY (header-only), applying the shared
        // dependency contract of deppath.h. Returns the existing texture path.
        fs::path resolvePlyTexture(const fs::path &ply, const std::string &rawName, const std::string &datasetRoot)
        {
            if (rawName.empty())
                throw AppException("PLY " + ply.string() + " has an empty TextureFile comment");

            if (deppath::isAbsolute(rawName))
                throw AppException("PLY " + ply.string() + " references the texture '" + rawName +
                                   "' by an absolute path, which can never be satisfied");

            const std::string name = deppath::normalize(rawName);
            if (name.empty() || name == "." || deppath::isAbsolute(name))
                throw AppException("PLY " + ply.string() + " has an invalid TextureFile '" + rawName + "'");

            const fs::path resolved = ply.parent_path() / fs::path(name);
            if (!datasetRoot.empty() && !deppath::isInside(resolved, fs::path(datasetRoot)))
                throw AppException("PLY " + ply.string() + " references the texture '" + rawName +
                                   "' outside the dataset, which can never be satisfied");

            std::error_code ec;
            if (!fs::is_regular_file(resolved, ec))
                throw BuildDepMissingException("Missing texture " + name + " for " + ply.filename().string(),
                                               name);
            return resolved;
        }

        // Only single-texture meshes are converted with their texture: with several
        // TextureFile comments or a per-face texnumber every face would silently be
        // mapped onto the first texture.
        void throwIfUnsupportedTexture(const fs::path &ply, const PlyHeader &h)
        {
            const std::string reason = plyUnsupportedTextureReason(h);
            if (!reason.empty())
                throw AppException("Cannot convert " + ply.filename().string() + " with its texture: " + reason);
        }

        // File name used for the copied texture next to the OBJ: the basename with
        // whitespace replaced (OBJ/MTL readers split `map_Kd` on whitespace).
        std::string sanitizedFilename(const fs::path &path)
        {
            std::string n = path.filename().string();
            for (auto &c : n)
                if (std::isspace(static_cast<unsigned char>(c)))
                    c = '_';
            return n;
        }

        bool sameFile(const fs::path &a, const fs::path &b)
        {
            if (fs::absolute(a).lexically_normal() == fs::absolute(b).lexically_normal())
                return true;
            std::error_code ec;
            return fs::equivalent(a, b, ec) && !ec;
        }

        // ------------------------------------------------------------------
        // Conversion plan (from the header only)
        // ------------------------------------------------------------------

        struct Plan
        {
            int vertexElement = -1;
            int faceElement = -1;

            int px = -1, py = -1, pz = -1;    // vertex position properties
            int cr = -1, cg = -1, cb = -1;    // vertex colour properties (all or none)
            int ts = -1, tt = -1;             // vertex texcoord properties (both or none)

            int faceIndices = -1;  // face vertex index list
            int faceTexcoord = -1; // face per-corner texcoord list

            bool perVertexTexcoords() const { return ts >= 0 && faceTexcoord < 0; }
            bool perFaceTexcoords() const { return faceTexcoord >= 0; }
            bool hasTexcoords() const { return perVertexTexcoords() || perFaceTexcoords(); }
            bool hasColors() const { return cr >= 0; }
        };

        std::string ctx(const fs::path &ply)
        {
            return "Cannot convert " + ply.filename().string() + " to OBJ: ";
        }

        bool isColorType(PlyScalarType t)
        {
            return t == PlyScalarType::UInt8 || t == PlyScalarType::UInt16 || t == PlyScalarType::Float32 ||
                   t == PlyScalarType::Float64;
        }

        Plan makePlan(const fs::path &ply, const PlyHeader &h)
        {
            if (h.format == PlyFormat::Unknown)
                throw AppException(ctx(ply) + "unsupported or missing format '" + h.formatName +
                                   "' (expected ascii, binary_little_endian or binary_big_endian)");
            if (h.version != "1.0")
                LOGD << "PLY " << ply.string() << " declares version '" << h.version << "', reading it as 1.0";

            Plan plan;
            plan.vertexElement = h.findElement("vertex");
            plan.faceElement = h.findElement("face");
            if (plan.vertexElement < 0)
                throw AppException(ctx(ply) + "no 'vertex' element");
            if (plan.faceElement < 0)
                throw AppException(ctx(ply) + "no 'face' element (not a mesh)");
            if (plan.faceElement < plan.vertexElement)
                throw AppException(ctx(ply) + "the 'face' element precedes the 'vertex' element (unsupported order)");

            // Every property read before the end of the face element must have a known type.
            for (int e = 0; e <= plan.faceElement; ++e)
            {
                for (const auto &p : h.elements[e].properties)
                {
                    if (p.type == PlyScalarType::Unknown || (p.isList && p.countType == PlyScalarType::Unknown))
                        throw AppException(ctx(ply) + "property '" + p.name + "' of element '" + h.elements[e].name +
                                           "' has an unknown type '" + typeNameOf(p) + "'");
                    if (p.isList && (p.countType == PlyScalarType::Float32 || p.countType == PlyScalarType::Float64))
                        LOGD << "PLY list '" << p.name << "' uses a floating-point count type";
                }
            }

            const PlyElement &ve = h.elements[plan.vertexElement];
            const auto scalar = [&ve](const char *name) {
                const int i = ve.findProperty(name);
                return (i >= 0 && !ve.properties[i].isList) ? i : -1;
            };

            plan.px = scalar("x");
            plan.py = scalar("y");
            plan.pz = scalar("z");
            if (plan.px < 0 || plan.py < 0 || plan.pz < 0)
                throw AppException(ctx(ply) + "the 'vertex' element lacks scalar x, y and z properties");

            static const char *colorSets[][3] = {
                {"red", "green", "blue"}, {"diffuse_red", "diffuse_green", "diffuse_blue"}, {"r", "g", "b"}};
            // The first complete set of a supported type wins. A complete set of an
            // unsupported type (e.g. int red/green/blue) does not stop the search;
            // when it is the only complete set the conversion fails rather than
            // silently dropping the colours.
            int unsupportedColor = -1;
            for (const auto &cs : colorSets)
            {
                const int c[3] = {scalar(cs[0]), scalar(cs[1]), scalar(cs[2])};
                if (c[0] < 0 || c[1] < 0 || c[2] < 0)
                    continue;
                int bad = -1;
                for (int k : c)
                    if (!isColorType(ve.properties[k].type))
                    {
                        bad = k;
                        break;
                    }
                if (bad < 0)
                {
                    plan.cr = c[0];
                    plan.cg = c[1];
                    plan.cb = c[2];
                    break;
                }
                if (unsupportedColor < 0)
                    unsupportedColor = bad;
            }
            if (plan.cr < 0 && unsupportedColor >= 0)
            {
                const PlyProperty &p = ve.properties[unsupportedColor];
                throw AppException(ctx(ply) + "vertex colour property '" + p.name + "' has the unsupported type '" +
                                   typeNameOf(p) + "' (supported: uchar, ushort, float, double)");
            }
            else if (unsupportedColor >= 0)
            {
                LOGD << "PLY " << ply.string() << ": ignoring vertex colour property '"
                     << ve.properties[unsupportedColor].name << "' of unsupported type '"
                     << typeNameOf(ve.properties[unsupportedColor]) << "'";
            }

            static const char *uvSets[][2] = {
                {"s", "t"}, {"u", "v"}, {"texture_u", "texture_v"}, {"texture_s", "texture_t"}};
            for (const auto &uv : uvSets)
            {
                const int s = scalar(uv[0]), t = scalar(uv[1]);
                if (s >= 0 && t >= 0)
                {
                    plan.ts = s;
                    plan.tt = t;
                    break;
                }
            }

            const PlyElement &fe = h.elements[plan.faceElement];
            for (const char *name : {"vertex_indices", "vertex_index"})
            {
                const int i = fe.findProperty(name);
                if (i >= 0 && fe.properties[i].isList)
                {
                    plan.faceIndices = i;
                    break;
                }
            }
            if (plan.faceIndices < 0)
                throw AppException(ctx(ply) + "the 'face' element lacks a 'vertex_indices' (or 'vertex_index') list");

            const int tc = fe.findProperty("texcoord");
            if (tc >= 0 && fe.properties[tc].isList)
                plan.faceTexcoord = tc;

            return plan;
        }

        // ------------------------------------------------------------------
        // Body streaming
        // ------------------------------------------------------------------

        class Converter
        {
        public:
            Converter(const fs::path &ply, const PlyHeader &h, const Plan &plan, BodyReader &reader, TextWriter &obj)
                : ply(ply), h(h), plan(plan), reader(reader), obj(obj)
            {
            }

            void run(PlyToObjResult &res)
            {
                minX = minY = minZ = std::numeric_limits<double>::max();
                maxX = maxY = maxZ = std::numeric_limits<double>::lowest();

                for (int e = 0; e <= plan.faceElement; ++e)
                {
                    if (e == plan.vertexElement)
                        readVertices();
                    else if (e == plan.faceElement)
                        readFaces();
                    else
                        skipElement(e);
                }

                if (triangles == 0)
                    throw AppException(ctx(ply) + "the mesh has no faces with at least 3 corners");

                res.vertexCount = vertexCount;
                res.texcoordCount = vtCount;
                res.hasColors = plan.hasColors();
                res.info.faceCount = triangles;
                res.info.hasBounds = vertexCount > 0;
                if (res.info.hasBounds)
                {
                    res.info.minX = minX;
                    res.info.minY = minY;
                    res.info.minZ = minZ;
                    res.info.maxX = maxX;
                    res.info.maxY = maxY;
                    res.info.maxZ = maxZ;
                }
                if (degenerate > 0)
                    LOGD << "Skipped " << degenerate << " face(s) with fewer than 3 corners in " << ply.string();
            }

        private:
            [[noreturn]] void fail(const PlyElement &e, uint64_t index, const PlyProperty &p, ReadStatus st)
            {
                const std::string where = "element '" + e.name + "' index " + std::to_string(index) + " (property '" +
                                          p.name + "')";
                if (st == ReadStatus::Eof)
                    throw AppException(ctx(ply) + "unexpected end of file in " + where + "; the file declares " +
                                       std::to_string(e.count) + " '" + e.name + "' entries");
                throw AppException(ctx(ply) + "malformed value '" + reader.lastToken() + "' in " + where);
            }

            double value(const PlyElement &e, uint64_t index, const PlyProperty &p, PlyScalarType t)
            {
                double v = 0;
                const ReadStatus st = reader.read(t, v);
                if (st != ReadStatus::Ok)
                    fail(e, index, p, st);
                return v;
            }

            uint64_t listCount(const PlyElement &e, uint64_t index, const PlyProperty &p)
            {
                const double c = value(e, index, p, p.countType);
                if (!(c >= 0) || c != std::floor(c) || c > static_cast<double>(std::numeric_limits<uint32_t>::max()))
                    throw AppException(ctx(ply) + "invalid list count " + std::to_string(c) + " in element '" +
                                       e.name + "' index " + std::to_string(index) + " (property '" + p.name + "')");
                return static_cast<uint64_t>(c);
            }

            void skipProperty(const PlyElement &e, uint64_t index, const PlyProperty &p)
            {
                if (!p.isList)
                {
                    value(e, index, p, p.type);
                    return;
                }
                const uint64_t n = listCount(e, index, p);
                for (uint64_t k = 0; k < n; ++k)
                    value(e, index, p, p.type);
            }

            void skipElement(int ei)
            {
                const PlyElement &e = h.elements[ei];
                for (uint64_t i = 0; i < e.count; ++i)
                    for (const auto &p : e.properties)
                        skipProperty(e, i, p);
            }

            void readVertices()
            {
                const PlyElement &e = h.elements[plan.vertexElement];
                vertexCount = e.count;
                const size_t np = e.properties.size();
                std::vector<double> vals(np, 0.0);

                const auto colorOut = [&](int pi) {
                    const PlyScalarType t = e.properties[pi].type;
                    const double v = vals[pi];
                    if (t == PlyScalarType::UInt8)
                        appendNumber(obj.buffer(), v / 255.0, 9);
                    else if (t == PlyScalarType::UInt16)
                        appendNumber(obj.buffer(), v / 65535.0, 9);
                    else
                        appendNumber(obj.buffer(), v, precisionFor(t));
                };

                const int xp = precisionFor(e.properties[plan.px].type);
                const int yp = precisionFor(e.properties[plan.py].type);
                const int zp = precisionFor(e.properties[plan.pz].type);

                for (uint64_t i = 0; i < e.count; ++i)
                {
                    for (size_t k = 0; k < np; ++k)
                    {
                        const PlyProperty &p = e.properties[k];
                        if (p.isList)
                            skipProperty(e, i, p);
                        else
                            vals[k] = value(e, i, p, p.type);
                    }

                    const double x = vals[plan.px], y = vals[plan.py], z = vals[plan.pz];
                    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
                        throw AppException(ctx(ply) + "vertex " + std::to_string(i) +
                                           " has a non-finite coordinate (properties x/y/z)");

                    const auto finiteGroup = [&](const char *group, std::initializer_list<int> properties) {
                        for (const int pi : properties)
                            if (!std::isfinite(vals[pi]))
                                throw AppException(ctx(ply) + "vertex " + std::to_string(i) +
                                                   " has a non-finite " + group + " (property '" +
                                                   e.properties[pi].name + "')");
                    };
                    if (plan.hasColors())
                        finiteGroup("color", {plan.cr, plan.cg, plan.cb});
                    if (plan.perVertexTexcoords())
                        finiteGroup("texcoord", {plan.ts, plan.tt});

                    minX = std::min(minX, x);
                    maxX = std::max(maxX, x);
                    minY = std::min(minY, y);
                    maxY = std::max(maxY, y);
                    minZ = std::min(minZ, z);
                    maxZ = std::max(maxZ, z);

                    std::string &b = obj.buffer();
                    b += "v ";
                    appendNumber(b, x, xp);
                    b += ' ';
                    appendNumber(b, y, yp);
                    b += ' ';
                    appendNumber(b, z, zp);
                    if (plan.hasColors())
                    {
                        b += ' ';
                        colorOut(plan.cr);
                        b += ' ';
                        colorOut(plan.cg);
                        b += ' ';
                        colorOut(plan.cb);
                    }
                    b += '\n';

                    if (plan.perVertexTexcoords())
                    {
                        b += "vt ";
                        appendNumber(b, vals[plan.ts], precisionFor(e.properties[plan.ts].type));
                        b += ' ';
                        appendNumber(b, vals[plan.tt], precisionFor(e.properties[plan.tt].type));
                        b += '\n';
                        ++vtCount;
                    }
                    obj.maybeFlush();
                }
            }

            void readFaces()
            {
                const PlyElement &e = h.elements[plan.faceElement];
                const bool perVertexTc = plan.perVertexTexcoords();
                const bool perFaceTc = plan.perFaceTexcoords();
                std::vector<uint64_t> corners;
                std::vector<double> tc;
                int tcPrecision = 9;
                if (perFaceTc)
                    tcPrecision = precisionFor(e.properties[plan.faceTexcoord].type);

                for (uint64_t i = 0; i < e.count; ++i)
                {
                    corners.clear();
                    tc.clear();
                    for (size_t k = 0; k < e.properties.size(); ++k)
                    {
                        const PlyProperty &p = e.properties[k];
                        if (static_cast<int>(k) == plan.faceIndices)
                        {
                            const uint64_t n = listCount(e, i, p);
                            if (n > kMaxFaceCorners)
                                throw AppException(ctx(ply) + "face " + std::to_string(i) + " declares " +
                                                   std::to_string(n) + " corners (limit " +
                                                   std::to_string(kMaxFaceCorners) + ")");
                            for (uint64_t c = 0; c < n; ++c)
                            {
                                const double v = value(e, i, p, p.type);
                                if (!(v >= 0) || v != std::floor(v) || v >= static_cast<double>(vertexCount))
                                {
                                    std::ostringstream vs;
                                    vs.imbue(std::locale::classic());
                                    vs << v;
                                    throw AppException(ctx(ply) + "face " + std::to_string(i) +
                                                       " references vertex index " + vs.str() +
                                                       " out of range (vertex count " +
                                                       std::to_string(vertexCount) + ")");
                                }
                                corners.push_back(static_cast<uint64_t>(v));
                            }
                        }
                        else if (static_cast<int>(k) == plan.faceTexcoord)
                        {
                            const uint64_t n = listCount(e, i, p);
                            if (n > 2 * kMaxFaceCorners)
                                throw AppException(ctx(ply) + "face " + std::to_string(i) + " declares " +
                                                   std::to_string(n) + " texture coordinates (limit " +
                                                   std::to_string(2 * kMaxFaceCorners) + ")");
                            for (uint64_t c = 0; c < n; ++c)
                                tc.push_back(value(e, i, p, p.type));
                        }
                        else
                        {
                            skipProperty(e, i, p);
                        }
                    }

                    const size_t n = corners.size();
                    if (n < 3)
                    {
                        ++degenerate;
                        continue;
                    }

                    if (perFaceTc && tc.size() != 2 * n)
                        throw AppException(ctx(ply) + "face " + std::to_string(i) + " has " +
                                           std::to_string(tc.size()) + " texcoord values, expected " +
                                           std::to_string(2 * n) + " (2 per corner)");
                    if (perFaceTc)
                        for (size_t c = 0; c < tc.size(); ++c)
                            if (!std::isfinite(tc[c]))
                                throw AppException(ctx(ply) + "face " + std::to_string(i) +
                                                   " has a non-finite texcoord (property '" +
                                                   e.properties[plan.faceTexcoord].name +
                                                   "', value " + std::to_string(c) + ")");

                    std::string &b = obj.buffer();
                    // Per-face texcoords: one vt per corner, in face order, right before the f lines.
                    const uint64_t vtBase = vtCount; // 0-based index of this face's first vt
                    if (perFaceTc)
                    {
                        for (size_t c = 0; c < n; ++c)
                        {
                            b += "vt ";
                            appendNumber(b, tc[2 * c], tcPrecision);
                            b += ' ';
                            appendNumber(b, tc[2 * c + 1], tcPrecision);
                            b += '\n';
                        }
                        vtCount += n;
                    }

                    const auto corner = [&](size_t c) {
                        appendUInt(b, corners[c] + 1);
                        if (perFaceTc)
                        {
                            b += '/';
                            appendUInt(b, vtBase + c + 1);
                        }
                        else if (perVertexTc)
                        {
                            b += '/';
                            appendUInt(b, corners[c] + 1);
                        }
                    };

                    // Fan triangulation: (c0, ci, ci+1). Exact for triangles and convex
                    // polygons, wrong for concave n-gons (documented limitation).
                    for (size_t c = 1; c + 1 < n; ++c)
                    {
                        b += "f ";
                        corner(0);
                        b += ' ';
                        corner(c);
                        b += ' ';
                        corner(c + 1);
                        b += '\n';
                        ++triangles;
                    }
                    obj.maybeFlush();
                }
            }

            const fs::path &ply;
            const PlyHeader &h;
            const Plan &plan;
            BodyReader &reader;
            TextWriter &obj;

            uint64_t vertexCount = 0;
            uint64_t vtCount = 0;
            uint64_t triangles = 0;
            uint64_t degenerate = 0;
            double minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0;
        };

        fs::path objPathFor(const std::string &outputBase)
        {
            fs::path p(outputBase);
            std::string ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".obj")
                return p;
            return fs::path(outputBase + ".obj");
        }

    } // namespace

    std::string getPlyTexture(const std::string &inputPly, const std::string &datasetRoot)
    {
        const fs::path ply(inputPly);
        if (!fs::exists(ply))
            throw FSException(inputPly + " does not exist");
        const PlyHeader h = readPlyHeader(ply);
        if (!h.hasTextureFile)
            return "";
        throwIfUnsupportedTexture(ply, h);
        return resolvePlyTexture(ply, h.textureFilename, datasetRoot).string();
    }

    PlyToObjResult convertPlyToObj(const std::string &inputPly, const std::string &outputBase,
                                   const PlyToObjOptions &opts)
    {
        const fs::path ply(inputPly);
        if (!fs::exists(ply))
            throw FSException(inputPly + " does not exist");

        // Header only: format, element order, property types and the texture
        // dependency are all checked before any body I/O.
        const PlyHeader h = readPlyHeader(ply);
        const Plan plan = makePlan(ply, h);

        fs::path texture;
        if (opts.withTexture)
        {
            throwIfUnsupportedTexture(ply, h);
            if (h.hasTextureFile)
                texture = resolvePlyTexture(ply, h.textureFilename, opts.datasetRoot);
        }

        PlyToObjResult res;
        const fs::path objPath = objPathFor(outputBase);
        if (!texture.empty() && sameFile(objPath, texture))
            throw AppException(ctx(ply) + "OBJ output would overwrite the source texture: " + objPath.string());
        const fs::path outDir = objPath.parent_path();
        if (!outDir.empty())
        {
            std::error_code ec;
            fs::create_directories(outDir, ec);
            if (ec)
                throw FSException("Cannot create " + outDir.string() + ": " + ec.message(), ec);
        }

        OutputGuard guard;
        res.objPath = objPath.string();

        std::string mtlName;
        if (!texture.empty())
        {
            fs::path mtlPath = outDir / sanitizedFilename(fs::path(objPath).replace_extension(".mtl"));
            // A source texture may have any extension. Choose a distinct material
            // filename before opening it, including when outputs share its folder.
            while (sameFile(mtlPath, texture) || sameFile(mtlPath, objPath))
                mtlPath = outDir / (mtlPath.stem().string() + "_material.mtl");
            std::string texName = sanitizedFilename(texture);
            fs::path texDst = outDir / texName;
            while (sameFile(texDst, objPath) || sameFile(texDst, mtlPath))
            {
                texName = "texture_" + texName;
                texDst = outDir / texName;
            }

            guard.add(mtlPath);
            {
                TextWriter mtl(mtlPath);
                mtl.buffer() += "# Generated by DroneDB from " + ply.filename().string() + "\n";
                mtl.buffer() += "newmtl material0\n";
                mtl.buffer() += "map_Kd " + texName + "\n";
                mtl.close();
            }

            std::error_code ec;
            const bool same = sameFile(texture, texDst);
            if (!same)
            {
                guard.add(texDst);
                fs::copy_file(texture, texDst, fs::copy_options::overwrite_existing, ec);
                if (ec)
                    throw FSException("Cannot copy texture " + texture.string() + " to " + texDst.string() + ": " +
                                      ec.message(), ec);
            }

            mtlName = mtlPath.filename().string();
            res.mtlPath = mtlPath.string();
            res.texturePath = texDst.string();
        }

        guard.add(objPath);
        {
            TextWriter obj(objPath);
            obj.buffer() += "# Converted from " + ply.filename().string() + " by DroneDB\n";
            if (!mtlName.empty())
            {
                obj.buffer() += "mtllib " + mtlName + "\n";
                obj.buffer() += "usemtl material0\n";
            }

            BodyReader reader(ply, h.headerLength, h.format);
            Converter conv(ply, h, plan, reader, obj);
            conv.run(res);
            obj.close();
        }

        guard.dismiss();
        LOGD << "Converted " << inputPly << " to " << res.objPath << " (" << res.vertexCount << " vertices, "
             << res.info.faceCount << " triangles" << (res.hasColors ? ", colours" : "")
             << (res.texcoordCount > 0 ? ", texcoords" : "") << (res.mtlPath.empty() ? "" : ", texture") << ")";
        return res;
    }

}
