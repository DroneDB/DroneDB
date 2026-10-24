/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "build_budget.h"

#include <cstdlib>
#include <mutex>
#include <optional>
#include <set>

#include "gdal_inc.h"
#include "logger.h"
#include "parallel.h"
#include "process_config.h"

namespace ddb {

namespace {

constexpr int64_t MiB = 1024 * 1024;
constexpr int DefaultCacheMb = 1024;
constexpr int DefaultWarpMemoryMb = 512;
constexpr int DefaultMaxThreads = 8;

// Positive integer config option, or `fallback` when unset or not a positive number.
int positiveOption(const char* name, int fallback) {
    const char* value = CPLGetConfigOption(name, nullptr);
    if (value == nullptr)
        return fallback;
    const int parsed = std::atoi(value);
    return parsed > 0 ? parsed : fallback;
}

// Shared state of all live ScopedGdalCacheMax guards, and of the size the host gave the cache.
struct CacheGuardState {
    std::mutex mutex;
    std::multiset<int64_t> requested;
    int64_t saved = 0;
    // GDAL's size before the first gdalCacheMb of DDBConfigure replaced it (the setting put
    // back to null restores it)
    std::optional<int64_t> beforeConfigure;
};

CacheGuardState& cacheGuardState() {
    static CacheGuardState state;
    return state;
}

}  // namespace

BuildResourceBudget BuildResourceBudget::fromConfig() {
    BuildResourceBudget budget{};
    budget.cacheBytes = static_cast<int64_t>(positiveOption("DDB_COG_CACHE_MB", DefaultCacheMb)) * MiB;
    budget.warpMemoryMb = positiveOption("DDB_WARP_MEMORY_MB", DefaultWarpMemoryMb);
    // 0 is a valid explicit value (all cores)
    budget.threads = parallel::threadsOption("DDB_BUILD_THREADS", DefaultMaxThreads);
    return budget;
}

namespace {

// The size the cache has when no guard is alive: GDAL's now, or the one a live guard will restore
int64_t baselineCache(const CacheGuardState& state) {
    return state.requested.empty() ? GDALGetCacheMax64() : state.saved;
}

void setBaselineCache(CacheGuardState& state, int64_t bytes) {
    if (state.requested.empty())
        GDALSetCacheMax64(bytes);
    else
        state.saved = bytes;
}

}  // namespace

void applyGdalCacheSetting() {
    auto& state = cacheGuardState();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto& config = ProcessConfig::get();
    const int64_t bytes = static_cast<int64_t>(config.gdalCacheMb()) * MiB;

    if (config.gdalCacheSource() == ConfigSource::Configure) {
        // The host chose the size: exactly that, raising or lowering GDAL's own
        if (!state.beforeConfigure)
            state.beforeConfigure = baselineCache(state);
        setBaselineCache(state, bytes);
        LOGD << "GDAL block cache set to " << config.gdalCacheMb() << " MB by DDBConfigure";
        return;
    }

    // From the environment or the default, an upper bound, as before DDBConfigure had the setting
    if (state.beforeConfigure) {
        setBaselineCache(state, *state.beforeConfigure);
        state.beforeConfigure.reset();
    }
    if (CPLGetConfigOption("GDAL_CACHEMAX", nullptr) != nullptr) {
        LOGD << "GDAL_CACHEMAX is set, keeping GDAL block cache at " << baselineCache(state) / MiB << " MB";
        return;
    }
    const int64_t current = baselineCache(state);
    if (current > bytes) {
        setBaselineCache(state, bytes);
        LOGD << "GDAL block cache bounded from " << current / MiB << " MB to " << bytes / MiB << " MB";
    }
}

ScopedGdalCacheMax::ScopedGdalCacheMax(int64_t bytes) : bytes_(bytes) {
    auto& state = cacheGuardState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.requested.empty())
        state.saved = GDALGetCacheMax64();
    state.requested.insert(bytes_);
    GDALSetCacheMax64(*state.requested.rbegin());
}

ScopedGdalCacheMax::~ScopedGdalCacheMax() {
    auto& state = cacheGuardState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.requested.erase(state.requested.find(bytes_));
    GDALSetCacheMax64(state.requested.empty() ? state.saved : *state.requested.rbegin());
}

}  // namespace ddb
