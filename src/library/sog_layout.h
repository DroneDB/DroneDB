/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef SOG_LAYOUT_H
#define SOG_LAYOUT_H

// Private to the library: the textures of an unbundled SOG splat (FR-UPL-09). Not installed, not
// part of the API.
//
// A SOG splat written as files (not bundled into one .sog) is a folder with a meta.json and the
// WebP textures it names (means_l, means_u, scales, quats, sh0, shN_centroids, shN_labels); the
// level-of-detail layout is a folder with a lod-meta.json and one such folder per chunk below it.
// Those textures are not photos: they are indexed as plain files (Generic).

#include <mutex>
#include <string>
#include <unordered_map>

#include "fs.h"

namespace ddb
{

    // The folders already looked at, for the files of one pass (an add, a rescan): what a
    // lod-meta.json or a meta.json says is read once per folder, not once per texture.
    // Shared by the threads of the pass.
    struct SogFolderCache
    {
        // Whether the folder holds a lod-meta.json of the LOD layout
        bool hasLodMeta(const fs::path &folder);
        // Whether the folder holds a meta.json of the SOG schema
        bool hasSogMeta(const fs::path &folder);

    private:
        std::mutex mutex_;
        std::unordered_map<std::string, bool> lodMeta_;
        std::unordered_map<std::string, bool> sogMeta_;
    };

    inline constexpr const char *SogLodMetaName = "lod-meta.json";
    inline constexpr const char *SogMetaName = "meta.json";

    // A .webp file named like a texture of a SOG (means_l.webp, ..., shN_labels.webp)
    bool isSogTextureName(const fs::path &path);

    // A file that makes the textures around it SOG textures: lod-meta.json or meta.json
    bool isSogMarkerName(const fs::path &path);

    // Whether `path` is a texture of an unbundled SOG under `rootDirectory`: a SOG texture name,
    // and a meta.json of the SOG schema in its folder or a lod-meta.json in its folder or in a
    // folder above it, up to the root. False when rootDirectory is empty or does not hold the
    // file (a file seen alone).
    bool isUnbundledSogTexture(const fs::path &path, const fs::path &rootDirectory, SogFolderCache &cache);

} // namespace ddb

#endif // SOG_LAYOUT_H
