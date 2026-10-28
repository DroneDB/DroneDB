/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "splat_format.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <initializer_list>

#include "build_layout.h"
#include "mio.h"
#include "ply.h"
#include "stringutil.h"

namespace ddb
{
    namespace
    {
        constexpr unsigned bit(SplatStage stage)
        {
            return 1u << static_cast<unsigned>(stage);
        }

        constexpr unsigned stages(std::initializer_list<SplatStage> list)
        {
            unsigned mask = 0;
            for (const SplatStage s : list)
                mask |= bit(s);
            return mask;
        }

        struct SplatFormatRow
        {
            SplatFormat format;
            const char *extension; // lower case, no dot
            unsigned stages;
            const char *mediaType; // nullptr: none of its own
        };

        // The table. The order is that of the extension lists built from it (the handler's, the
        // converter's help text), so it is the order a user reads them in.
        constexpr std::array<SplatFormatRow, 6> kFormats = {{
            // Told from a point cloud or a mesh by its header, not by its extension
            {SplatFormat::Ply, "ply",
             stages({SplatStage::Metadata, SplatStage::Build, SplatStage::Convert}), nullptr},
            {SplatFormat::Splat, "splat",
             stages({SplatStage::Detect, SplatStage::Metadata, SplatStage::Build, SplatStage::Convert}),
             GsplatMediaType},
            {SplatFormat::Spz, "spz",
             stages({SplatStage::Detect, SplatStage::Metadata, SplatStage::Build, SplatStage::Thumbnail,
                     SplatStage::Convert}),
             GsplatMediaType},
            {SplatFormat::Ksplat, "ksplat",
             stages({SplatStage::Detect, SplatStage::Metadata, SplatStage::Build}), GsplatMediaType},
            // A single-file ZIP bundle build-lod reads. No metadata: the count is in a meta.json
            // inside the ZIP, which indexing does not open
            {SplatFormat::Sog, "sog", stages({SplatStage::Detect, SplatStage::Build}), GsplatMediaType},
            // What a build writes; as an upload it is not accepted as a source yet
            {SplatFormat::Rad, "rad", stages({SplatStage::Thumbnail}), GsplatMediaType},
        }};

        const SplatFormatRow *rowOf(SplatFormat format)
        {
            for (const auto &row : kFormats)
                if (row.format == format)
                    return &row;
            return nullptr;
        }

        // Reads up to `size` bytes from the start of the file; returns how many it got.
        size_t readHead(const fs::path &path, uint8_t *out, size_t size)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in.good())
                return 0;
            in.read(reinterpret_cast<char *>(out), static_cast<std::streamsize>(size));
            return static_cast<size_t>(in.gcount());
        }

        uint16_t le16(const uint8_t *p)
        {
            uint16_t v;
            std::memcpy(&v, p, sizeof(v));
            return v;
        }

        uint32_t le32(const uint8_t *p)
        {
            uint32_t v;
            std::memcpy(&v, p, sizeof(v));
            return v;
        }

        // SPZ header: magic "NGSP", version, point count, SH degree. A v4 file starts with it; a
        // legacy file has it as the first 16 bytes of what its gzip stream inflates to.
        constexpr size_t kSpzHeaderSize = 16;
        constexpr uint32_t kSpzMagic = 0x5053474e; // 'NGSP'

        bool spzHeaderFrom(const uint8_t *hdr, SpzContainer container, SpzHeader &header)
        {
            if (le32(hdr) != kSpzMagic)
                return false;
            header.container = container;
            header.version = le32(hdr + 4);
            header.numPoints = le32(hdr + 8);
            header.shDegree = static_cast<int>(hdr[12]);
            return header.numPoints > 0;
        }

        // Inflates the first `size` bytes of the gzip stream at the start of a file, reading the
        // file only as far as they need. False when the stream does not have that many bytes.
        bool inflateGzipPrefix(const fs::path &path, uint8_t *out, size_t size)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in.good())
                return false;

            z_stream stream = {};
            if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) // 16: a gzip header, not zlib's
                return false;

            std::array<uint8_t, 4096> chunk;
            stream.next_out = out;
            stream.avail_out = static_cast<uInt>(size);
            int ret = Z_OK;
            while (stream.avail_out > 0 && ret == Z_OK)
            {
                if (stream.avail_in == 0)
                {
                    in.read(reinterpret_cast<char *>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
                    const auto got = in.gcount();
                    if (got <= 0)
                        break;
                    stream.next_in = chunk.data();
                    stream.avail_in = static_cast<uInt>(got);
                }
                ret = inflate(&stream, Z_NO_FLUSH);
            }
            const bool filled = stream.avail_out == 0;
            inflateEnd(&stream);
            return filled;
        }

        constexpr size_t kKsplatHeaderSize = 4096;
        constexpr size_t kKsplatSectionSize = 1024;
        constexpr int kMaxShDegree = 3;
    } // namespace

    SplatFormat splatFormatOfExtension(std::string_view extension)
    {
        if (!extension.empty() && extension.front() == '.')
            extension.remove_prefix(1);
        for (const auto &row : kFormats)
            if (str::iequals(extension, row.extension))
                return row.format;
        return SplatFormat::Unknown;
    }

    SplatFormat splatFormatOf(const fs::path &path)
    {
        return splatFormatOfExtension(io::lowerExtension(path));
    }

    bool splatFormatAccepts(SplatFormat format, SplatStage stage)
    {
        const SplatFormatRow *row = rowOf(format);
        return row != nullptr && (row->stages & bit(stage)) != 0;
    }

    bool splatAccepts(const fs::path &path, SplatStage stage)
    {
        return splatFormatAccepts(splatFormatOf(path), stage);
    }

    std::vector<std::string> splatExtensions(SplatStage stage)
    {
        std::vector<std::string> extensions;
        for (const auto &row : kFormats)
            if ((row.stages & bit(stage)) != 0)
                extensions.emplace_back(row.extension);
        return extensions;
    }

    std::string splatExtensionsText(SplatStage stage)
    {
        const auto extensions = splatExtensions(stage);
        std::string text;
        for (size_t i = 0; i < extensions.size(); ++i)
        {
            if (i > 0)
                text += (i + 1 == extensions.size()) ? " or " : ", ";
            text += "." + extensions[i];
        }
        return text;
    }

    const char *splatMediaType(SplatFormat format)
    {
        const SplatFormatRow *row = rowOf(format);
        return row != nullptr ? row->mediaType : nullptr;
    }

    SpzContainer sniffSpzContainer(const uint8_t *bytes, size_t size)
    {
        if (size >= 4 && le32(bytes) == kSpzMagic)
            return SpzContainer::Ngsp;
        if (size >= 2 && bytes[0] == 0x1F && bytes[1] == 0x8B)
            return SpzContainer::Gzip;
        return SpzContainer::None;
    }

    SpzContainer sniffSpzContainer(const fs::path &path)
    {
        uint8_t head[4] = {0};
        return sniffSpzContainer(head, readHead(path, head, sizeof(head)));
    }

    bool readSpzHeader(const fs::path &path, SpzHeader &header)
    {
        header = SpzHeader();
        uint8_t hdr[kSpzHeaderSize] = {0};
        const size_t got = readHead(path, hdr, sizeof(hdr));
        switch (sniffSpzContainer(hdr, got))
        {
        case SpzContainer::Ngsp:
            return got >= kSpzHeaderSize && spzHeaderFrom(hdr, SpzContainer::Ngsp, header);
        case SpzContainer::Gzip:
            return inflateGzipPrefix(path, hdr, sizeof(hdr)) && spzHeaderFrom(hdr, SpzContainer::Gzip, header);
        default:
            return false;
        }
    }

    bool readKsplatHeader(const fs::path &path, KsplatHeader &header)
    {
        header = KsplatHeader();

        std::error_code ec;
        const uint64_t fileSize = fs::file_size(path, ec);
        if (ec || fileSize < kKsplatHeaderSize)
            return false;

        std::ifstream in(path, std::ios::binary);
        std::array<uint8_t, kKsplatHeaderSize> head;
        in.read(reinterpret_cast<char *>(head.data()), static_cast<std::streamsize>(head.size()));
        if (!in.good())
            return false;

        // Version 0.1 and later, as the build-lod reader takes it
        if (head[0] != 0 || head[1] < 1)
            return false;
        const uint64_t sectionCount = le32(head.data() + 4);
        const uint64_t numSplats = le32(head.data() + 16);
        if (numSplats == 0 || kKsplatHeaderSize + sectionCount * kKsplatSectionSize > fileSize)
            return false;

        // The SH degree of a splat is its section's: the file's is the highest of them
        int shDegree = 0;
        for (uint64_t i = 0; i < sectionCount; ++i)
        {
            uint8_t field[2];
            in.seekg(static_cast<std::streamoff>(kKsplatHeaderSize + i * kKsplatSectionSize + 40));
            in.read(reinterpret_cast<char *>(field), sizeof(field));
            if (!in.good())
                return false;
            shDegree = std::max<int>(shDegree, le16(field));
        }
        if (shDegree > kMaxShDegree)
            return false;

        header.numSplats = numSplats;
        header.shDegree = shDegree;
        return true;
    }

    bool looksLikeSplat(const fs::path &path)
    {
        switch (splatFormatOf(path))
        {
        case SplatFormat::Ply:
        {
            try
            {
                PlyHeader header;
                return parsePlyHeader(path, header, nullptr) && looksLikeSplatPly(header);
            }
            catch (const std::exception &)
            {
                return false; // the file cannot be opened
            }
        }
        case SplatFormat::Splat:
        {
            std::error_code ec;
            const auto size = fs::file_size(path, ec);
            return !ec && size > 0 && (size % SplatRecordSize) == 0;
        }
        case SplatFormat::Spz:
            return sniffSpzContainer(path) != SpzContainer::None;
        case SplatFormat::Ksplat:
        {
            KsplatHeader header;
            return readKsplatHeader(path, header);
        }
        case SplatFormat::Sog:
        {
            uint8_t head[4] = {0};
            return readHead(path, head, sizeof(head)) == sizeof(head) && le32(head) == 0x04034b50; // PK\3\4
        }
        case SplatFormat::Rad:
        {
            uint8_t head[4] = {0};
            return readHead(path, head, sizeof(head)) == sizeof(head) && le32(head) == 0x30444152; // RAD0
        }
        case SplatFormat::Unknown:
        default:
            return false;
        }
    }

} // namespace ddb
