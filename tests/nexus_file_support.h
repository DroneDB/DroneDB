/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef NEXUS_FILE_SUPPORT_H
#define NEXUS_FILE_SUPPORT_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "fs.h"

namespace testutils
{
    /// What the tests read of a Nexus (.nxs) file: the header, the encoded node textures, the
    /// texture of each node and which nodes are leaves
    struct NexusFile
    {
        uint32_t magic = 0; // 'N' 'x' 's' ' ' in a little-endian word: 0x4e787320
        uint64_t nvert = 0;
        uint64_t nface = 0;
        uint32_t nNodes = 0; // with the sink, the last node
        uint32_t nPatches = 0;
        uint32_t nTextures = 0; // with the end marker, the last texture
        std::vector<std::vector<uint8_t>> textures; // one per texture, as stored (padding included)
        std::vector<int> nodeTexture;               // the texture of each node but the sink
        std::vector<bool> leaf;                     // nodes whose patches all point to the sink
        std::vector<float> error;                   // the error of each node but the sink
        std::vector<std::array<float, 3>> center;   // the center of the bounding sphere of each node but the sink
    };

    /// Reads the file; a test failure, and what could be read, when it is too short
    NexusFile readNexusFile(const fs::path &path);

    /// The encoded texture is a WebP image
    bool isWebp(const std::vector<uint8_t> &texture);
} // namespace testutils

#endif // NEXUS_FILE_SUPPORT_H
