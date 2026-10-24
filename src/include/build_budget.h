/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef BUILD_BUDGET_H
#define BUILD_BUDGET_H

#include <cstdint>


namespace ddb {

// Resources one raster build may use. Read in one place from the config
// options below (environment variables or CPLSetConfigOption):
//   DDB_COG_CACHE_MB    GDAL block cache while the build runs (default 1024)
//   DDB_WARP_MEMORY_MB  GDALWarp working memory, -wm (default 512)
//   DDB_BUILD_THREADS   GDAL threads wanted; 0 = all cores (default min(8, cores)).
//                       The CPU token pool (parallel.h) decides how many run.
struct BuildResourceBudget {
    int64_t cacheBytes;
    int warpMemoryMb;
    int threads;  // >= 1

    static BuildResourceBudget fromConfig();
};

// Applies the gdalCacheMb setting (process_config.h) to GDAL's process-wide block cache (GDAL
// default: 5% of RAM). Set with DDBConfigure, it is the size of the cache: exactly that, at once
// (a ScopedGdalCacheMax guard alive keeps its size, and the cache takes the new one when the
// last guard ends). From DDB_GDAL_CACHE_MB or the default (1024 MB) it is a bound: it lowers the
// cache, never raises it, and does nothing when GDAL_CACHEMAX is set explicitly. The setting put
// back to null undoes what DDBConfigure did, then bounds the cache as above.
// Called by ddb::registerProcess, and by ProcessConfig::configure when the setting changes.
void applyGdalCacheSetting();

// Sets GDAL's block cache to `bytes` while the guard is alive. The cache is
// process-wide, so all guards share one state: while any guard is alive the
// cache is the largest value requested by the live guards, and the value that
// was in effect before the first guard is restored when the last one ends.
// This keeps concurrent builds bounded: one build ending never restores the
// unbounded default under another build that is still running.
class ScopedGdalCacheMax {
public:
    explicit ScopedGdalCacheMax(int64_t bytes);
    ~ScopedGdalCacheMax();

    ScopedGdalCacheMax(const ScopedGdalCacheMax&) = delete;
    ScopedGdalCacheMax& operator=(const ScopedGdalCacheMax&) = delete;

private:
    int64_t bytes_;
};

}  // namespace ddb

#endif  // BUILD_BUDGET_H
