/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef PLY_H
#define PLY_H

#include <cstdint>
#include <vector>
#include <string>
#include "fs.h"
#include "entry_types.h"
#include "ddb_export.h"

namespace ddb
{

    // ---------------------------------------------------------------------
    // PLY header model
    //
    // One parser shared by identifyPly(), getPlyInfo() and the streaming
    // PLY -> OBJ converter (plyconv.h). The header is read in binary mode,
    // a trailing '\r' is stripped from every line (CRLF headers), and lines
    // are tokenised on whitespace: keywords are matched as whole tokens at
    // the start of the line, so text inside a `comment` never adds elements
    // or properties.
    // ---------------------------------------------------------------------

    enum class PlyFormat
    {
        Unknown,             // `format` line missing or not one of the three below
        Ascii,               // format ascii 1.0
        BinaryLittleEndian,  // format binary_little_endian 1.0
        BinaryBigEndian      // format binary_big_endian 1.0
    };

    enum class PlyScalarType
    {
        Unknown, // unrecognised type name (kept so that name-only consumers still work)
        Int8,    // char, int8
        UInt8,   // uchar, uint8
        Int16,   // short, int16
        UInt16,  // ushort, uint16
        Int32,   // int, int32
        UInt32,  // uint, uint32
        Float32, // float, float32
        Float64  // double, float64
    };

    // Maps a PLY type name (including the sized aliases) to its scalar type;
    // PlyScalarType::Unknown when not recognised.
    DDB_DLL PlyScalarType plyScalarTypeFromName(const std::string &name);
    // Size in bytes of a binary scalar (0 for Unknown).
    DDB_DLL size_t plyScalarSize(PlyScalarType t);

    struct PlyProperty
    {
        std::string name;
        // Scalar type of the property; for a list, the type of each item
        // (e.g. the vertex index type of `vertex_indices`).
        PlyScalarType type = PlyScalarType::Unknown;
        bool isList = false;
        // Type of the per-instance item count (lists only).
        PlyScalarType countType = PlyScalarType::Unknown;
        // Original type names as written in the header (for error messages).
        std::string typeName;
        std::string countTypeName;
    };

    struct PlyElement
    {
        std::string name;
        uint64_t count = 0;
        std::vector<PlyProperty> properties; // in header order

        // Index of the property named `name`, or -1.
        int findProperty(const std::string &propName) const
        {
            for (size_t i = 0; i < properties.size(); ++i)
                if (properties[i].name == propName)
                    return static_cast<int>(i);
            return -1;
        }
    };

    struct PlyHeader
    {
        PlyFormat format = PlyFormat::Unknown;
        std::string formatName; // raw token after `format` (e.g. "binary_little_endian")
        std::string version;    // raw version token (e.g. "1.0")
        std::vector<PlyElement> elements; // in header (and body) order
        std::vector<std::string> comments; // text after `comment `, trimmed, in order
        std::vector<std::string> objInfo;  // text after `obj_info `, trimmed, in order
        // True when a `comment TextureFile ...` line exists.
        bool hasTextureFile = false;
        // Text after `comment TextureFile ` (rest of the line, trimmed) of the first
        // such comment; empty when there is none.
        std::string textureFilename;
        // Every `comment TextureFile` name, in header order (MeshLab writes one per
        // texture of a multi-texture mesh). textureFilenames.front() == textureFilename.
        std::vector<std::string> textureFilenames;
        // Number of bytes from the start of the file up to and including the line
        // terminator of `end_header`: the body starts at this offset.
        uint64_t headerLength = 0;

        // Index of the element named `name`, or -1.
        int findElement(const std::string &elementName) const
        {
            for (size_t i = 0; i < elements.size(); ++i)
                if (elements[i].name == elementName)
                    return static_cast<int>(i);
            return -1;
        }
    };

    // Parses the header of a PLY file. Returns false (with a reason in `error`
    // when provided) if the file is not a PLY, the header is malformed (property
    // before any element, non-numeric element count, ...), longer than the
    // safety cap (512 lines) or has no `end_header`. Unknown keywords are ignored;
    // unknown format/type names are recorded as Unknown rather than rejected, so
    // that the name-only consumers (getPlyInfo) keep working. Consumers that read
    // the body (plyconv) validate them.
    // @throws FSException if the file cannot be opened.
    DDB_DLL bool parsePlyHeader(const fs::path &plyFile, PlyHeader &header, std::string *error = nullptr);

    // Same as parsePlyHeader but throws AppException with the reason on failure.
    DDB_DLL PlyHeader readPlyHeader(const fs::path &plyFile);

    // Multi-texture meshes are not supported (only one texture is converted, so a
    // face of texture #2 would silently be mapped onto texture #1). Returns a
    // human-readable reason when the header declares more than one
    // `comment TextureFile` or the `face` element has a `texnumber` property;
    // empty when the texture layout is supported.
    DDB_DLL std::string plyUnsupportedTextureReason(const PlyHeader &header);

    struct PlyInfo
    {
        unsigned long vertexCount;
        bool isMesh;
        bool hasTextures;
        // True when the vertex element carries 3D Gaussian Splat attributes
        // (spherical-harmonic DC term and/or anisotropic scale + rotation + opacity)
        // and there is no face element. Mutually exclusive with isMesh.
        bool isSplat;
        // Spherical-harmonics degree (0..3) inferred from the number of f_rest_*
        // properties. -1 when the file is not a Gaussian Splat.
        int shDegree;
        // Flat list of every property name across all elements, in header order.
        std::vector<std::string> dimensions;
        // Value of `comment TextureFile <name>` (rest of line, trimmed); empty when
        // absent. hasTextures is true exactly when a TextureFile comment exists.
        std::string textureFilename;
        // Number of `comment TextureFile` lines.
        size_t textureFileCount;
        // Non-empty when the texture layout is unsupported (multi-texture: several
        // TextureFile comments or a per-face `texnumber`); see
        // plyUnsupportedTextureReason(). Such a mesh is identified as Generic.
        std::string unsupportedTextureReason;
    };

    DDB_DLL EntryType identifyPly(const fs::path &plyFile);
    DDB_DLL bool getPlyInfo(const fs::path &plyFile, PlyInfo &info);

}
#endif // PLY_H
