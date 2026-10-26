/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "nexus_file_support.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

#include "gtest/gtest.h"
#include "mio.h"

namespace testutils
{
    namespace
    {
        // The layout of a Nexus file: an 88 byte header, then a table of 44 byte nodes, one of 12
        // byte patches and one of 68 byte textures (whose first word is the offset of the texture
        // in 256 byte units)
        constexpr size_t headerSize = 88, nodeSize = 44, patchSize = 12, textureSize = 68;

        template <typename T> T readAt(const std::vector<uint8_t> &b, size_t at)
        {
            T v{};
            if (at + sizeof(T) <= b.size())
                std::memcpy(&v, b.data() + at, sizeof(T));
            return v;
        }
    } // namespace

    NexusFile readNexusFile(const fs::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        const std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        NexusFile f;
        EXPECT_GE(b.size(), headerSize) << ddb::io::toUtf8(path);
        if (b.size() < headerSize)
            return f;
        f.magic = readAt<uint32_t>(b, 0);
        f.nvert = readAt<uint64_t>(b, 8);
        f.nface = readAt<uint64_t>(b, 16);
        f.nNodes = readAt<uint32_t>(b, 60);
        f.nPatches = readAt<uint32_t>(b, 64);
        f.nTextures = readAt<uint32_t>(b, 68);

        const size_t nodes = headerSize, patches = nodes + nodeSize * f.nNodes, textures = patches + patchSize * f.nPatches;
        std::vector<uint64_t> offsets;
        for (uint32_t t = 0; t < f.nTextures; t++)
            offsets.push_back(uint64_t(readAt<uint32_t>(b, textures + textureSize * t)) * 256);
        for (size_t t = 0; t + 1 < offsets.size(); t++)
        {
            const uint64_t end = std::min<uint64_t>(offsets[t + 1], b.size());
            const uint64_t begin = std::min<uint64_t>(offsets[t], end);
            f.textures.emplace_back(b.begin() + std::ptrdiff_t(begin), b.begin() + std::ptrdiff_t(end));
        }

        const uint32_t sink = f.nNodes > 0 ? f.nNodes - 1 : 0;
        for (uint32_t n = 0; n < sink; n++)
        {
            const auto first = readAt<uint32_t>(b, nodes + nodeSize * n + 40);
            const auto end = readAt<uint32_t>(b, nodes + nodeSize * (n + 1) + 40);
            bool leaf = true;
            for (uint32_t p = first; p < end; p++)
                leaf = leaf && readAt<uint32_t>(b, patches + patchSize * p) == sink;
            f.leaf.push_back(leaf);
            f.error.push_back(readAt<float>(b, nodes + nodeSize * n + 8));
            f.center.push_back({readAt<float>(b, nodes + nodeSize * n + 20), readAt<float>(b, nodes + nodeSize * n + 24),
                                readAt<float>(b, nodes + nodeSize * n + 28)});
            f.nodeTexture.push_back(f.nTextures > 0 ? int(readAt<uint32_t>(b, patches + patchSize * first + 8)) : -1);
        }
        return f;
    }

    bool isWebp(const std::vector<uint8_t> &texture)
    {
        return texture.size() >= 12 && std::memcmp(texture.data(), "RIFF", 4) == 0 &&
               std::memcmp(texture.data() + 8, "WEBP", 4) == 0;
    }
} // namespace testutils
