/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "build_budget.h"

#include <cstdlib>
#include <mutex>
#include <set>

#include "gdal_inc.h"
#include "logger.h"
#include "parallel.h"

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

// Shared state of all live ScopedGdalCacheMax guards.
struct CacheGuardState {
    std::mutex mutex;
    std::multiset<int64_t> requested;
    int64_t saved = 0;
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

std::string BuildResourceBudget::gdalNumThreads() const {
    return threads == 0 ? "ALL_CPUS" : std::to_string(threads);
}

void applyDefaultGdalCacheBound() {
    if (CPLGetConfigOption("GDAL_CACHEMAX", nullptr) != nullptr) {
        LOGD << "GDAL_CACHEMAX is set, keeping GDAL block cache at " << GDALGetCacheMax64() / MiB << " MB";
        return;
    }
    const int64_t bound = static_cast<int64_t>(positiveOption("DDB_GDAL_CACHE_MB", DefaultCacheMb)) * MiB;
    const int64_t current = GDALGetCacheMax64();
    if (current > bound) {
        GDALSetCacheMax64(bound);
        LOGD << "GDAL block cache bounded from " << current / MiB << " MB to " << bound / MiB << " MB";
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
