/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef DSM_CACHE_H
#define DSM_CACHE_H

// The two caches of the DSM service (dsmservice.h): the rasters held in memory, and the folder of
// downloaded rasters on the disk.

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "dsm_reader.h"
#include "fs.h"

namespace ddb
{

    // File name --> raster. The mutex guards every access and is held only around the map
    // itself: the callers recurse through the loaders, so no lock may be held across them.
    class DsmMemoryCache
    {
        std::unordered_map<std::string, DSMCacheEntry> entries;
        std::mutex mutex;

    public:
        // The elevation from a loaded raster containing the point (0 when the DSM has no value
        // there), or nothing when no loaded raster contains it
        std::optional<float> lookup(double latitude, double longitude);
        bool contains(const std::string &filename);
        void put(const std::string &filename, DSMCacheEntry entry);
        void erase(const std::string &filename);
    };

    // Reads the raster and keeps it in memory under its file name (with its pixels only when it
    // covers the point, so a file outside the query is not opened again). True when it covers
    // the point.
    bool addRasterToCache(DsmMemoryCache &memory, const fs::path &filePath,
                          double latitude, double longitude);

    // Reads the rasters of the folder that memory does not hold yet until one covers the point
    // (true). A raster that cannot be opened is deleted.
    bool loadDsmFolder(const fs::path &folder, DsmMemoryCache &memory,
                       double latitude, double longitude);

} // namespace ddb

#endif // DSM_CACHE_H
