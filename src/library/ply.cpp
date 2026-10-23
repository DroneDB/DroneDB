/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <fstream>
#include <algorithm>
#include <cstdint>
#include <istream>
#include "logger.h"
#include "stringutil.h"
#include "ply.h"
#include "exceptions.h"
#include "mio.h"

namespace ddb
{

    namespace
    {
        // Determine whether a vertex-only PLY carries 3D Gaussian Splat attributes and,
        // if so, its spherical-harmonics degree. Operates on the property names already
        // collected in info.dimensions. A splat is never a mesh, so callers must check
        // info.isMesh first.
        void computePlySplatInfo(PlyInfo &info)
        {
            if (info.isMesh)
                return;

            const auto &dims = info.dimensions;
            const auto has = [&dims](const std::string &name) {
                return std::find(dims.begin(), dims.end(), name) != dims.end();
            };

            // Primary signature: the spherical-harmonics DC term.
            // Fallback signature: anisotropic covariance attributes (scale + rotation + opacity),
            // which a few exporters emit without an explicit f_dc_* term.
            const bool primary = has("f_dc_0");
            const bool fallback = has("scale_0") && has("scale_1") && has("scale_2") &&
                                  has("rot_0") && has("rot_1") && has("rot_2") && has("rot_3") &&
                                  has("opacity");

            if (!primary && !fallback)
                return;

            info.isSplat = true;

            // Spherical-harmonics degree from the count of f_rest_* properties.
            // Per-channel higher-order coeffs x 3 channels:
            //   0 -> degree 0, 9 -> degree 1, 24 -> degree 2, 45 -> degree 3.
            int fRestCount = 0;
            for (const auto &d : dims)
            {
                if (d.starts_with("f_rest_"))
                    ++fRestCount;
            }

            switch (fRestCount)
            {
                case 0:  info.shDegree = 0; break;
                case 9:  info.shDegree = 1; break;
                case 24: info.shDegree = 2; break;
                case 45: info.shDegree = 3; break;
                default:
                    // Unknown/partial SH layout: clamp to the closest lower standard degree
                    // so downstream consumers still get a sane value.
                    if (fRestCount >= 45)      info.shDegree = 3;
                    else if (fRestCount >= 24) info.shDegree = 2;
                    else if (fRestCount >= 9)  info.shDegree = 1;
                    else                       info.shDegree = 0;
                    break;
            }
        }

        // Longest header line accepted; protects against binary files that happen to
        // start with "ply\n" but have no line structure.
        constexpr size_t kMaxHeaderLineLength = 64 * 1024;

        // Gaussian Splat headers can be long (degree-4 spherical harmonics carry
        // 72 f_rest_* properties), so allow a generous but bounded number of lines.
        constexpr int kMaxHeaderLines = 512;

        // Reads one '\n'-terminated line, adding the consumed bytes (terminator
        // included) to `consumed`. A trailing '\r' is stripped (CRLF headers).
        // Returns false at end of file with nothing read, or when the line is
        // longer than kMaxHeaderLineLength (`tooLong` is then set).
        bool readHeaderLine(std::istream &in, std::string &line, uint64_t &consumed, bool &tooLong)
        {
            line.clear();
            tooLong = false;
            bool any = false;
            std::streambuf *sb = in.rdbuf();
            while (true)
            {
                const int c = sb->sbumpc();
                if (c == std::char_traits<char>::eof())
                    break;
                any = true;
                ++consumed;
                if (c == '\n')
                    break;
                if (line.size() >= kMaxHeaderLineLength)
                {
                    tooLong = true;
                    return false;
                }
                line.push_back(static_cast<char>(c));
            }
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            return any;
        }

        // Text after the first `n` whitespace-separated tokens, trimmed.
        std::string restAfterTokens(const std::string &line, int n)
        {
            size_t i = 0;
            for (int k = 0; k < n; ++k)
            {
                while (i < line.size() && str::isSpaceChar(line[i]))
                    ++i;
                while (i < line.size() && !str::isSpaceChar(line[i]))
                    ++i;
            }
            while (i < line.size() && str::isSpaceChar(line[i]))
                ++i;
            size_t end = line.size();
            while (end > i && str::isSpaceChar(line[end - 1]))
                --end;
            return line.substr(i, end - i);
        }

        bool parseCount(const std::string &s, uint64_t &out)
        {
            if (s.empty() || s.size() > 19)
                return false;
            uint64_t v = 0;
            for (char c : s)
            {
                if (c < '0' || c > '9')
                    return false;
                v = v * 10 + static_cast<uint64_t>(c - '0');
            }
            out = v;
            return true;
        }

        bool parseHeaderStream(std::istream &in, PlyHeader &h, std::string &error)
        {
            h = PlyHeader();
            uint64_t consumed = 0;
            bool tooLong = false;
            std::string line;

            if (!readHeaderLine(in, line, consumed, tooLong) || line != "ply")
            {
                error = "not a PLY file (missing 'ply' magic line)";
                return false;
            }

            int i = 0;
            while (readHeaderLine(in, line, consumed, tooLong))
            {
                const auto tokens = str::splitWhitespace(line);
                if (!tokens.empty())
                {
                    const std::string &kw = tokens[0];
                    if (kw == "end_header")
                    {
                        h.headerLength = consumed;
                        return true;
                    }
                    else if (kw == "format")
                    {
                        h.formatName = tokens.size() > 1 ? tokens[1] : "";
                        h.version = tokens.size() > 2 ? tokens[2] : "";
                        if (h.formatName == "ascii")
                            h.format = PlyFormat::Ascii;
                        else if (h.formatName == "binary_little_endian")
                            h.format = PlyFormat::BinaryLittleEndian;
                        else if (h.formatName == "binary_big_endian")
                            h.format = PlyFormat::BinaryBigEndian;
                        else
                            h.format = PlyFormat::Unknown;
                    }
                    else if (kw == "comment")
                    {
                        h.comments.push_back(restAfterTokens(line, 1));
                        if (tokens.size() > 1 && tokens[1] == "TextureFile")
                        {
                            h.textureFilenames.push_back(restAfterTokens(line, 2));
                            if (!h.hasTextureFile)
                            {
                                h.hasTextureFile = true;
                                h.textureFilename = h.textureFilenames.back();
                            }
                        }
                    }
                    else if (kw == "obj_info")
                    {
                        h.objInfo.push_back(restAfterTokens(line, 1));
                    }
                    else if (kw == "element")
                    {
                        PlyElement e;
                        if (tokens.size() < 3)
                        {
                            error = "malformed element line: '" + line + "'";
                            return false;
                        }
                        e.name = tokens[1];
                        if (!parseCount(tokens[2], e.count))
                        {
                            error = "malformed count for element '" + e.name + "': '" + tokens[2] + "'";
                            return false;
                        }
                        if (e.name == "vertex")
                        {
                            if (e.count == 0)
                                LOGW << "PLY file contains zero vertices";
                            if (e.count > 1000000000) // 1 billion vertices seems excessive
                                LOGW << "PLY file reports extremely large vertex count: " << e.count;
                        }
                        h.elements.push_back(std::move(e));
                    }
                    else if (kw == "property")
                    {
                        if (h.elements.empty())
                        {
                            error = "property declared before any element: '" + line + "'";
                            return false;
                        }
                        PlyProperty p;
                        if (tokens.size() >= 2 && tokens[1] == "list")
                        {
                            if (tokens.size() < 5)
                            {
                                error = "malformed list property line: '" + line + "'";
                                return false;
                            }
                            p.isList = true;
                            p.countTypeName = tokens[2];
                            p.countType = plyScalarTypeFromName(tokens[2]);
                            p.typeName = tokens[3];
                            p.type = plyScalarTypeFromName(tokens[3]);
                            p.name = tokens[4];
                        }
                        else
                        {
                            if (tokens.size() < 3)
                            {
                                error = "malformed property line: '" + line + "'";
                                return false;
                            }
                            p.typeName = tokens[1];
                            p.type = plyScalarTypeFromName(tokens[1]);
                            p.name = tokens[2];
                        }
                        h.elements.back().properties.push_back(std::move(p));
                    }
                    // Any other keyword is ignored.
                }

                if (i++ > kMaxHeaderLines)
                {
                    error = "header longer than the parser limit";
                    return false;
                }
            }

            error = tooLong ? "header line too long" : "missing end_header";
            return false;
        }
    } // namespace

    PlyScalarType plyScalarTypeFromName(const std::string &name)
    {
        if (name == "char" || name == "int8")
            return PlyScalarType::Int8;
        if (name == "uchar" || name == "uint8")
            return PlyScalarType::UInt8;
        if (name == "short" || name == "int16")
            return PlyScalarType::Int16;
        if (name == "ushort" || name == "uint16")
            return PlyScalarType::UInt16;
        if (name == "int" || name == "int32")
            return PlyScalarType::Int32;
        if (name == "uint" || name == "uint32")
            return PlyScalarType::UInt32;
        if (name == "float" || name == "float32")
            return PlyScalarType::Float32;
        if (name == "double" || name == "float64")
            return PlyScalarType::Float64;
        return PlyScalarType::Unknown;
    }

    size_t plyScalarSize(PlyScalarType t)
    {
        switch (t)
        {
            case PlyScalarType::Int8:
            case PlyScalarType::UInt8:
                return 1;
            case PlyScalarType::Int16:
            case PlyScalarType::UInt16:
                return 2;
            case PlyScalarType::Int32:
            case PlyScalarType::UInt32:
            case PlyScalarType::Float32:
                return 4;
            case PlyScalarType::Float64:
                return 8;
            default:
                return 0;
        }
    }

    bool parsePlyHeader(const fs::path &plyFile, PlyHeader &header, std::string *error)
    {
        std::ifstream in(plyFile, std::ios::binary);
        if (!in.is_open())
            throw FSException("Cannot open " + io::toUtf8(plyFile));

        std::string err;
        const bool ok = parseHeaderStream(in, header, err);
        if (!ok && error != nullptr)
            *error = err;
        return ok;
    }

    PlyHeader readPlyHeader(const fs::path &plyFile)
    {
        PlyHeader h;
        std::string err;
        if (!parsePlyHeader(plyFile, h, &err))
            throw AppException("Invalid PLY header in " + io::toUtf8(plyFile) + ": " + err);
        return h;
    }

    std::string plyUnsupportedTextureReason(const PlyHeader &header)
    {
        if (header.textureFilenames.size() > 1)
            return "the header declares " + std::to_string(header.textureFilenames.size()) +
                   " TextureFile comments (multi-texture PLY meshes are not supported)";
        // A texnumber column without any TextureFile is just an untextured mesh
        const int face = header.findElement("face");
        if (!header.textureFilenames.empty() && face >= 0 && header.elements[face].findProperty("texnumber") >= 0)
            return "the 'face' element has a per-face 'texnumber' property (multi-texture PLY meshes are not supported)";
        return "";
    }

    DDB_DLL bool getPlyInfo(const fs::path &plyFile, PlyInfo &info)
    {
        info.isMesh = false;      // Assumed
        info.hasTextures = false; // Assumed
        info.isSplat = false;     // Assumed
        info.shDegree = -1;       // Not a splat
        info.dimensions.clear();
        info.vertexCount = 0;
        info.textureFilename.clear();
        info.textureFileCount = 0;
        info.unsupportedTextureReason.clear();

        PlyHeader h;
        std::string err;
        if (!parsePlyHeader(plyFile, h, &err))
        {
            LOGD << "Cannot read PLY header of " << io::toUtf8(plyFile) << ": " << err;
            return false;
        }

        for (const auto &e : h.elements)
        {
            if (e.name == "vertex")
                info.vertexCount = static_cast<unsigned long>(e.count);
            else if (e.name == "face")
                info.isMesh = true;
            for (const auto &p : e.properties)
                info.dimensions.push_back(p.name);
        }
        info.hasTextures = h.hasTextureFile;
        info.textureFilename = h.textureFilename;
        info.textureFileCount = h.textureFilenames.size();
        info.unsupportedTextureReason = plyUnsupportedTextureReason(h);

        computePlySplatInfo(info);
        return true;
    }

    EntryType identifyPly(const fs::path &plyFile)
    {
        PlyInfo info;
        if (getPlyInfo(plyFile, info))
        {
            if (info.isMesh)
            {
                // Textured meshes are Models too: the build converts them to
                // OBJ + MTL + texture (plyconv.h) before Nexus, which the B4
                // spike verified (plyconv.texturedPlyBuildsNexusF1).
                // Multi-texture meshes (several TextureFile comments or a
                // per-face texnumber) cannot be converted faithfully: they
                // stay Generic, like any other unsupported textured PLY.
                if (!info.unsupportedTextureReason.empty())
                {
                    LOGD << "PLY " << io::toUtf8(plyFile) << " is not built as a Model: "
                         << info.unsupportedTextureReason;
                    return EntryType::Generic;
                }
                return EntryType::Model;
            }
            // A 3D Gaussian Splat is a vertex-only PLY carrying SH/covariance
            // attributes. Check before falling back to a plain point cloud.
            if (info.isSplat)
            {
                return EntryType::GaussianSplat;
            }
            return EntryType::PointCloud;
        }
        else
            return EntryType::Generic;
    }

}
