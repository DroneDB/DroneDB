/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CACHE_CLEANUP_H
#define CACHE_CLEANUP_H

#include <cstdint>
#include <map>
#include <string>

#include "json_fwd.h"

namespace ddb
{
    // How old (in days, by the last change of an entry) what the cleanup removes is
    struct CacheCleanupOptions
    {
        // The folders of rendered tiles (UserProfile::getTilesDir), level by level
        int tileDays = 5;
        // The copies of remote rasters in the tiles folder (a download of a URL to tile)
        int downloadDays = 7;
        // The elevation tiles of the DSM service (photos indexed without a relative altitude)
        int dsmDays = 30;
        // What a crash left: the folders of requests to the raster helper (<cache>/requests) that no
        // request of this process holds, and libddb's folders in the temporary folder (temp_names.h)
        int tempDays = 2;
        // Count what would go, remove nothing
        bool dryRun = false;
    };

    // What one kind of entry of the cleanup took: entries (files or folders) and bytes of their files
    struct CacheCleanupCount
    {
        uint64_t entries = 0;
        uint64_t bytes = 0;
    };

    // Per kind: "tiles", "downloads", "dsm", "requests", "temp", and "artefacts" (the artefact cache of an
    // earlier version, removed whatever its age)
    struct CacheCleanupResult
    {
        bool dryRun = false;
        std::map<std::string, CacheCleanupCount> kinds;
    };

    // Removes from the cache of libddb (ProcessConfig::cacheDir) and from the temporary folder what was not
    // changed for the days of `options`. The cache is shared by the processes of the user and the requests of
    // this one: an entry that vanishes meanwhile is skipped, one that cannot be removed (a reader holds it on
    // Windows) stays for the next time. Can be canceled between entries.
    CacheCleanupResult cleanupCache(const CacheCleanupOptions &options = {});

    // {"tileDays", "downloadDays", "dsmDays", "tempDays": whole numbers from 0 to 36500, "dryRun": bool}, each
    // optional. @throws InvalidArgsException
    CacheCleanupOptions cacheCleanupOptionsFromJson(const json &options);

    // {"dryRun", "kinds": {kind: {"entries", "bytes"}}}
    json toJson(const CacheCleanupResult &result);
} // namespace ddb

#endif // CACHE_CLEANUP_H
