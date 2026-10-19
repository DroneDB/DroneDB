/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef BUILD_BUDGET_H
#define BUILD_BUDGET_H

#include <cstdint>
#include <string>

#include "ddb_export.h"

namespace ddb {

// Resources one raster build may use. Read in one place from the config
// options below (environment variables or CPLSetConfigOption):
//   DDB_COG_CACHE_MB    GDAL block cache while the build runs (default 1024)
//   DDB_WARP_MEMORY_MB  GDALWarp working memory, -wm (default 512)
//   DDB_BUILD_THREADS   GDAL NUM_THREADS; 0 = all cores (default min(8, cores))
struct BuildResourceBudget {
    int64_t cacheBytes;
    int warpMemoryMb;
    int threads;

    DDB_DLL static BuildResourceBudget fromConfig();

    // Value for the GDAL NUM_THREADS option: "ALL_CPUS" or the thread count.
    DDB_DLL std::string gdalNumThreads() const;
};

// Bounds GDAL's process-wide block cache (GDAL default: 5% of RAM) to
// DDB_GDAL_CACHE_MB (default 1024 MB). Never raises it, and does nothing
// when GDAL_CACHEMAX is set explicitly. Called once by DDBRegisterProcess.
DDB_DLL void applyDefaultGdalCacheBound();

// Sets GDAL's block cache to `bytes` while the guard is alive. The cache is
// process-wide, so all guards share one state: while any guard is alive the
// cache is the largest value requested by the live guards, and the value that
// was in effect before the first guard is restored when the last one ends.
// This keeps concurrent builds bounded: one build ending never restores the
// unbounded default under another build that is still running.
class DDB_DLL ScopedGdalCacheMax {
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
