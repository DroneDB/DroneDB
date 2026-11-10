/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// The textures of an unbundled SOG splat (FR-UPL-09): see sog_layout.h.

#include "sog_layout.h"

#include <algorithm>
#include <array>
#include <fstream>

#include "json.h"
#include "mio.h"

namespace ddb
{

    namespace
    {

        // The texture names a SOG writer gives (splat-transform: meta.json lists them in the
        // "files" of means, scales, quats, sh0 and shN)
        constexpr std::array<const char *, 7> SogTextureStems = {
            "means_l", "means_u", "scales", "quats", "sh0", "shN_centroids", "shN_labels"};

        // A SOG meta.json is a few kilobytes (its codebooks); a bigger file is not one
        constexpr std::uintmax_t SogMetaMaxBytes = 4u * 1024u * 1024u;

        bool isRegularFile(const fs::path &file)
        {
            std::error_code ec;
            return fs::is_regular_file(file, ec) && !ec;
        }

        // Reads a lod-meta.json only as far as it needs: the file is a JSON object and one of its
        // own keys is "lodLevels" or "filenames" (the LOD layout of splat-transform). A lod-meta.json
        // lists every chunk, so it can be tens of megabytes: the read stops at that key.
        class LodMetaReader final : public nlohmann::json_sax<json>
        {
            int depth_ = 0;

        public:
            bool isLodMeta = false;

            bool null() override { return depth_ > 0; }
            bool boolean(bool) override { return depth_ > 0; }
            bool number_integer(number_integer_t) override { return depth_ > 0; }
            bool number_unsigned(number_unsigned_t) override { return depth_ > 0; }
            bool number_float(number_float_t, const string_t &) override { return depth_ > 0; }
            bool string(string_t &) override { return depth_ > 0; }
            bool binary(binary_t &) override { return depth_ > 0; }

            bool start_object(std::size_t) override
            {
                depth_++;
                return true;
            }

            bool key(string_t &val) override
            {
                if (depth_ == 1 && (val == "lodLevels" || val == "filenames"))
                {
                    isLodMeta = true;
                    return false; // enough: stop reading
                }
                return true;
            }

            bool end_object() override
            {
                depth_--;
                return true;
            }

            // The top of the file is an object, or the file is not a lod-meta.json
            bool start_array(std::size_t) override
            {
                if (depth_ == 0)
                    return false;
                depth_++;
                return true;
            }

            bool end_array() override
            {
                depth_--;
                return true;
            }

            bool parse_error(std::size_t, const std::string &, const nlohmann::detail::exception &) override
            {
                return false;
            }
        };

        bool readLodMeta(const fs::path &file)
        {
            if (!isRegularFile(file))
                return false;
            std::ifstream in(file, std::ios::binary);
            if (!in)
                return false;
            LodMetaReader reader;
            json::sax_parse(in, &reader, json::input_format_t::json, false);
            return reader.isLodMeta;
        }

        // The schema of a SOG meta.json (version 1 and 2): one object for each of the texture
        // groups means, scales, quats and sh0, each with the list of its files
        bool readSogMeta(const fs::path &file)
        {
            if (!isRegularFile(file))
                return false;
            std::error_code ec;
            const auto size = fs::file_size(file, ec);
            if (ec || size > SogMetaMaxBytes)
                return false;

            std::ifstream in(file, std::ios::binary);
            const json meta = json::parse(in, nullptr, false);
            if (!meta.is_object())
                return false;
            for (const char *group : {"means", "scales", "quats", "sh0"})
            {
                const auto it = meta.find(group);
                if (it == meta.end() || !it->is_object())
                    return false;
                const auto files = it->find("files");
                if (files == it->end() || !files->is_array())
                    return false;
            }
            return true;
        }

        bool cached(std::mutex &mutex, std::unordered_map<std::string, bool> &decisions, const fs::path &folder,
                    bool (*read)(const fs::path &), const char *fileName)
        {
            const std::string key = io::toUtf8(folder);
            {
                std::lock_guard<std::mutex> lock(mutex);
                const auto it = decisions.find(key);
                if (it != decisions.end())
                    return it->second;
            }
            // Read without the lock: two threads may read the same file once each, never wrongly
            const bool found = read(folder / fileName);
            std::lock_guard<std::mutex> lock(mutex);
            decisions.emplace(key, found);
            return found;
        }

    } // namespace

    bool SogFolderCache::hasLodMeta(const fs::path &folder)
    {
        return cached(mutex_, lodMeta_, folder, readLodMeta, SogLodMetaName);
    }

    bool SogFolderCache::hasSogMeta(const fs::path &folder)
    {
        return cached(mutex_, sogMeta_, folder, readSogMeta, SogMetaName);
    }

    bool isSogTextureName(const fs::path &path)
    {
        if (!io::Path(path).checkExtension({"webp"}))
            return false;
        const std::string stem = io::toUtf8(path.stem());
        for (const char *name : SogTextureStems)
            if (stem == name)
                return true;
        return false;
    }

    bool isSogMarkerName(const fs::path &path)
    {
        const std::string name = io::toUtf8(path.filename());
        return name == SogLodMetaName || name == SogMetaName;
    }

    bool isUnbundledSogTexture(const fs::path &path, const fs::path &rootDirectory, SogFolderCache &cache)
    {
        if (rootDirectory.empty() || !isSogTextureName(path))
            return false;

        const std::string relative = io::Path(path).relativeTo(rootDirectory).generic();
        // Not under the root (a file given alone): no layout to look for
        if (relative.empty() || relative.rfind("..", 0) == 0)
            return false;

        fs::path folder = path.parent_path();
        // One chunk: the meta.json that names the textures is next to them
        if (cache.hasSogMeta(folder))
            return true;

        // The LOD layout: a lod-meta.json in the folder of the texture or in one above it, up to
        // the root
        const auto depth = std::count(relative.begin(), relative.end(), '/');
        for (std::ptrdiff_t i = 0; i <= depth; i++)
        {
            if (cache.hasLodMeta(folder))
                return true;
            if (!folder.has_parent_path() || folder == folder.parent_path())
                break;
            folder = folder.parent_path();
        }
        return false;
    }

} // namespace ddb
