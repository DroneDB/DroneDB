/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef SPLAT_FORMAT_H
#define SPLAT_FORMAT_H

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fs.h"

// The raw integer and float reads of the splat formats (the header fields, the .splat records)
// take the bytes as they are on disk, little-endian.
static_assert(std::endian::native == std::endian::little, "the splat readers assume a little-endian host");

namespace ddb
{
    // The Gaussian splat file formats. Every place that decides what a file extension means for
    // a splat (the entry handler, the metadata reader, the build, the thumbnail generator, the
    // converter, STAC) reads the table of splat_format.cpp: a new format is a row there and its
    // decoder.
    enum class SplatFormat
    {
        Unknown,
        Ply,    // 3DGS PLY (INRIA layout) or SuperSplat compressed PLY
        Splat,  // antimatter15 binary .splat (SplatRecordSize bytes per splat, SH degree 0)
        Spz,    // Niantic SPZ: gzip (v1-3) or NGSP/ZSTD (v4)
        Ksplat, // mkkellogg .ksplat
        Sog,    // SOGS bundle: a ZIP with meta.json and the WebP images of the splats
        Rad     // Spark RAD level-of-detail container (what a build produces)
    };

    // What a format is accepted for.
    enum class SplatStage
    {
        Detect,    // the extension alone makes the file a GaussianSplat entry (a PLY is told apart by
                   // its header, by the model handler)
        Metadata,  // getGaussianSplatInfo reads the splat count and SH degree from the file
        Build,     // buildGsplat can turn it into model.rad (build-lod reads it)
        Thumbnail, // a thumbnail is rendered from this file
        Convert    // `ddb gsplat` converts it to .spz
    };

    // Size of a record of an antimatter15 .splat: position 12, scale 12, colour 4, rotation 4.
    constexpr size_t SplatRecordSize = 32;

    // The splat format a file extension names, "spz" or ".SPZ" (case does not matter).
    SplatFormat splatFormatOfExtension(std::string_view extension);
    SplatFormat splatFormatOf(const fs::path &path);

    // Whether the format is accepted for the stage; false for Unknown.
    bool splatFormatAccepts(SplatFormat format, SplatStage stage);
    // The same for the format a path's extension names.
    bool splatAccepts(const fs::path &path, SplatStage stage);

    // The extensions (lower case, no dot) accepted for the stage, in the order of the table.
    std::vector<std::string> splatExtensions(SplatStage stage);

    // The same for a message: ".ply, .splat or .spz" (with the dots, "or" before the last one).
    std::string splatExtensionsText(SplatStage stage);

    // The media type of the format's files (STAC, the build manifest), nullptr when the format
    // has none of its own (a PLY is an ordinary PLY).
    const char *splatMediaType(SplatFormat format);

    // The container of an SPZ, told by its first bytes: a legacy SPZ (v1-3) is a gzip stream, a
    // v4 file starts with the NGSP magic.
    enum class SpzContainer
    {
        None,
        Gzip,
        Ngsp
    };
    SpzContainer sniffSpzContainer(const uint8_t *bytes, size_t size);
    SpzContainer sniffSpzContainer(const fs::path &path); // None when it cannot be read

    // True when the content is what the extension says, for the formats whose content can be
    // told without reading it all: the SPZ container, a size that is a whole number of .splat
    // records, the PLY header of a splat, the .ksplat header, the ZIP of a .sog, the RAD magic.
    // Never throws; false when the file cannot be read.
    bool looksLikeSplat(const fs::path &path);

    // What the first bytes of an SPZ say. For a legacy file only the first 16 bytes of the gzip
    // stream are inflated, so this stays cheap however large the file is.
    struct SpzHeader
    {
        SpzContainer container = SpzContainer::None;
        uint32_t version = 0;
        uint64_t numPoints = 0;
        int shDegree = 0;
    };
    // False when the file is not an SPZ, or when its header is not one (no magic, no points).
    bool readSpzHeader(const fs::path &path, SpzHeader &header);

    // What the header of a .ksplat says (4096 bytes and one 1024-byte header per section).
    struct KsplatHeader
    {
        uint64_t numSplats = 0;
        int shDegree = 0; // the highest of its sections
    };
    // False when the file is not a .ksplat of a version this reads, or its sections do not fit.
    bool readKsplatHeader(const fs::path &path, KsplatHeader &header);

} // namespace ddb

#endif // SPLAT_FORMAT_H
