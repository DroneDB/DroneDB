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

        void applyFormatLine(PlyHeader &h, const std::vector<std::string> &tokens)
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

        void applyCommentLine(PlyHeader &h, const std::string &line, const std::vector<std::string> &tokens)
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

        // An "element <name> <count>" line; false (with error) when it is malformed
        bool applyElementLine(PlyHeader &h, const std::string &line, const std::vector<std::string> &tokens,
                              std::string &error)
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
            return true;
        }

        // A "property [list <count type>] <type> <name>" line of the last element; false (with error)
        // when it is malformed or comes before any element
        bool applyPropertyLine(PlyHeader &h, const std::string &line, const std::vector<std::string> &tokens,
                               std::string &error)
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
                    bool wellFormed = true;
                    if (kw == "end_header")
                    {
                        h.headerLength = consumed;
                        return true;
                    }
                    else if (kw == "format")
                        applyFormatLine(h, tokens);
                    else if (kw == "comment")
                        applyCommentLine(h, line, tokens);
                    else if (kw == "element")
                        wellFormed = applyElementLine(h, line, tokens, error);
                    else if (kw == "property")
                        wellFormed = applyPropertyLine(h, line, tokens, error);
                    // Any other keyword is ignored.
                    if (!wellFormed)
                        return false;
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

    namespace
    {
        const PlyElement *elementNamed(const PlyHeader &h, const char *name)
        {
            const int i = h.findElement(name);
            return i < 0 ? nullptr : &h.elements[i];
        }

        bool hasProperty(const PlyElement &e, const std::string &name)
        {
            return e.findProperty(name) >= 0;
        }

        // The f_rest_* properties build-lod counts: f_rest_0, f_rest_1, ... up to the first
        // one that is missing
        int fRestCount(const PlyElement &e)
        {
            int n = 0;
            while (hasProperty(e, "f_rest_" + std::to_string(n)))
                ++n;
            return n;
        }

        // Spherical-harmonics degree for a count of f_rest_* properties (per-channel
        // higher-order coefficients x 3 channels: 0 -> 0, 9 -> 1, 24 -> 2, 45 -> 3); -1 when
        // the count is none of those
        int shDegreeOfFRestCount(int count)
        {
            switch (count)
            {
                case 0: return 0;
                case 9: return 1;
                case 24: return 2;
                case 45: return 3;
                default: return -1;
            }
        }

        std::string fRestProblem(int count)
        {
            return "f_rest count " + std::to_string(count) + " is not 0, 9, 24 or 45";
        }

        // The names of `required` the element lacks, comma separated
        std::string missingProperties(const PlyElement &e, const std::vector<std::string> &required)
        {
            std::string missing;
            for (const auto &name : required)
            {
                if (hasProperty(e, name))
                    continue;
                missing += (missing.empty() ? "" : ", ") + name;
            }
            return missing;
        }

        // The scalar type names build-lod reads (its parse_property_type): not the sized aliases
        bool isTypeNameOfBuildLod(const std::string &name)
        {
            return name == "char" || name == "uchar" || name == "short" || name == "ushort" ||
                   name == "int" || name == "uint" || name == "float" || name == "double";
        }

        // SuperSplat stores one chunk record per 256 splats
        constexpr uint64_t kSuperSplatChunkSize = 256;

        // The standard layout: x, y, z, f_dc_0..2, opacity, scale_0..2 and rot_0..3 on the vertex
        std::string standardSplatProblem(const PlyElement &vertex)
        {
            const std::string missing = missingProperties(
                vertex, {"x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2", "opacity", "scale_0", "scale_1", "scale_2",
                         "rot_0", "rot_1", "rot_2", "rot_3"});
            if (!missing.empty())
                return "Splat PLY is missing properties: " + missing;
            const int rest = fRestCount(vertex);
            return shDegreeOfFRestCount(rest) < 0 ? fRestProblem(rest) : std::string();
        }

        // The SuperSplat compressed layout: per-chunk bounds, packed vertices, optional `sh`
        std::string compressedSplatProblem(const PlyHeader &h, const PlyElement &vertex, const PlyElement &chunk)
        {
            const uint64_t chunksNeeded = (vertex.count + kSuperSplatChunkSize - 1) / kSuperSplatChunkSize;
            if (chunk.count < chunksNeeded)
                return "Compressed splat PLY has " + std::to_string(chunk.count) + " chunk records for " +
                       std::to_string(vertex.count) + " splats (at least " + std::to_string(chunksNeeded) +
                       " are needed)";

            std::string missing = missingProperties(chunk, {"min_x", "min_y", "min_z", "max_x", "max_y", "max_z",
                                                            "min_scale_x", "min_scale_y", "min_scale_z",
                                                            "max_scale_x", "max_scale_y", "max_scale_z"});
            if (!missing.empty())
                return "Compressed splat PLY is missing chunk properties: " + missing;
            missing = missingProperties(vertex, {"packed_position", "packed_rotation", "packed_scale", "packed_color"});
            if (!missing.empty())
                return "Compressed splat PLY is missing properties: " + missing;

            if (const PlyElement *sh = elementNamed(h, "sh"))
            {
                if (sh->count != vertex.count)
                    return "Compressed splat PLY has " + std::to_string(sh->count) + " sh records for " +
                           std::to_string(vertex.count) + " splats";
                const int rest = fRestCount(*sh);
                if (shDegreeOfFRestCount(rest) < 0)
                    return fRestProblem(rest);
            }
            return std::string();
        }

        // The layout part of the problem, once the file format is known to be readable
        std::string layoutProblem(const PlyHeader &h)
        {
            const PlyElement *vertex = elementNamed(h, "vertex");
            if (vertex == nullptr)
                return "Splat PLY has no vertex element";
            if (const PlyElement *chunk = elementNamed(h, "chunk"))
                return compressedSplatProblem(h, *vertex, *chunk);
            return standardSplatProblem(*vertex);
        }

        // Degree of the splat the header describes; -1 when the count of f_rest_* is none of the
        // standard ones or there is no vertex element
        int splatShDegree(const PlyHeader &h)
        {
            const PlyElement *vertex = elementNamed(h, "vertex");
            if (vertex == nullptr)
                return -1;
            if (elementNamed(h, "chunk") != nullptr)
            {
                const PlyElement *sh = elementNamed(h, "sh");
                return sh == nullptr ? 0 : shDegreeOfFRestCount(fRestCount(*sh));
            }
            return shDegreeOfFRestCount(fRestCount(*vertex));
        }
    } // namespace

    bool looksLikeSplatPly(const PlyHeader &h)
    {
        // A splat is never a mesh
        if (h.findElement("face") >= 0)
            return false;
        const PlyElement *vertex = elementNamed(h, "vertex");
        if (vertex == nullptr)
            return false;

        // The SuperSplat compressed layout
        if (elementNamed(h, "chunk") != nullptr && hasProperty(*vertex, "packed_position"))
            return true;

        // Primary signature: the spherical-harmonics DC term.
        // Fallback signature: anisotropic covariance attributes (scale + rotation + opacity),
        // which a few exporters emit without an explicit f_dc_* term.
        return hasProperty(*vertex, "f_dc_0") ||
               missingProperties(*vertex, {"scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2", "rot_3",
                                           "opacity"}).empty();
    }

    std::string plySplatBuildProblem(const PlyHeader &h)
    {
        switch (h.format)
        {
            case PlyFormat::BinaryLittleEndian:
                break;
            case PlyFormat::Ascii:
                return "ASCII splat PLY is not supported: export it as binary little-endian";
            case PlyFormat::BinaryBigEndian:
                return "Big-endian splat PLY is not supported: export it as binary little-endian";
            default:
                return h.formatName.empty()
                           ? "The splat PLY header has no format line"
                           : "PLY format '" + h.formatName + "' is not supported: export it as binary little-endian";
        }
        if (h.version != "1.0")
            return "PLY version '" + h.version + "' is not supported: export it as version 1.0";

        for (const auto &e : h.elements)
        {
            for (const auto &p : e.properties)
            {
                if (p.isList)
                    return "Splat PLY has the list property '" + p.name + "' in element '" + e.name +
                           "', which is not supported";
                if (!isTypeNameOfBuildLod(p.typeName))
                    return "Property '" + p.name + "' has the type '" + p.typeName +
                           "', which is not supported: use char, uchar, short, ushort, int, uint, float or double";
            }
        }
        return layoutProblem(h);
    }

    bool isBuildableSplatPly(const PlyHeader &header)
    {
        return plySplatBuildProblem(header).empty();
    }

    bool getPlyInfo(const fs::path &plyFile, PlyInfo &info)
    {
        info.isMesh = false;      // Assumed
        info.hasTextures = false; // Assumed
        info.isSplat = false;     // Assumed
        info.shDegree = -1;       // Not a splat
        info.splatProblem.clear();
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
                info.vertexCount = e.count;
            else if (e.name == "face")
                info.isMesh = true;
            for (const auto &p : e.properties)
                info.dimensions.push_back(p.name);
        }
        info.hasTextures = h.hasTextureFile;
        info.textureFilename = h.textureFilename;
        info.textureFileCount = h.textureFilenames.size();
        info.unsupportedTextureReason = plyUnsupportedTextureReason(h);

        if (!info.isMesh && looksLikeSplatPly(h))
        {
            info.isSplat = true;
            info.shDegree = splatShDegree(h);
            info.splatProblem = plySplatBuildProblem(h);
        }
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
                // OBJ + MTL + texture (plyconv.h) before bm3d, which the
                // plyconv.texturedPlyBuildsBm3d test verifies.
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
