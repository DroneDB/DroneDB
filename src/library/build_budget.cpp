/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "build_budget.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <optional>

#include "exceptions.h"
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

// The size the host gave GDAL's cache
struct CacheSettingState {
    std::mutex mutex;
    // GDAL's size before the first gdalCacheMb of DDBConfigure replaced it (the setting put
    // back to null restores it)
    std::optional<int64_t> beforeConfigure;
};

CacheSettingState& cacheSettingState() {
    static CacheSettingState state;
    return state;
}

}  // namespace

std::string BuildOptions::threadsRule() {
    return "threads must be an integer from 1 to " + std::to_string(MaxThreads);
}

BuildOptions BuildOptions::withThreads(int64_t threads) {
    if (threads < 1 || threads > MaxThreads)
        throw InvalidArgsException(threadsRule());
    BuildOptions options;
    options.threads = static_cast<int>(threads);
    return options;
}

BuildOptions BuildOptions::forSlice(double begin, double end) const {
    BuildOptions slice = *this;
    if (progress) {
        slice.progress = [outer = progress, begin, end](double fraction, std::string_view phase) {
            outer(fraction < 0.0 ? begin : begin + std::min(fraction, 1.0) * (end - begin), phase);
        };
    }
    return slice;
}

BuildResourceBudget BuildResourceBudget::fromConfig(const BuildOptions& options) {
    BuildResourceBudget budget{};
    budget.cacheBytes = static_cast<int64_t>(positiveOption("DDB_COG_CACHE_MB", DefaultCacheMb)) * MiB;
    budget.warpMemoryMb = positiveOption("DDB_WARP_MEMORY_MB", DefaultWarpMemoryMb);
    // 0 is a valid explicit value (all cores)
    budget.threads = parallel::threadsOption("DDB_BUILD_THREADS", DefaultMaxThreads);
    if (options.threads)
        budget.threads = std::min(budget.threads, *options.threads);
    return budget;
}

void applyGdalCacheSetting() {
    auto& state = cacheSettingState();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto& config = ProcessConfig::get();
    const int64_t bytes = static_cast<int64_t>(config.gdalCacheMb()) * MiB;

    if (config.gdalCacheSource() == ConfigSource::Configure) {
        // The host chose the size: exactly that, raising or lowering GDAL's own
        if (!state.beforeConfigure)
            state.beforeConfigure = GDALGetCacheMax64();
        GDALSetCacheMax64(bytes);
        LOGD << "GDAL block cache set to " << config.gdalCacheMb() << " MB by DDBConfigure";
        return;
    }

    // From the environment or the default, an upper bound, as before DDBConfigure had the setting
    if (state.beforeConfigure) {
        GDALSetCacheMax64(*state.beforeConfigure);
        state.beforeConfigure.reset();
    }
    if (CPLGetConfigOption("GDAL_CACHEMAX", nullptr) != nullptr) {
        LOGD << "GDAL_CACHEMAX is set, keeping GDAL block cache at " << GDALGetCacheMax64() / MiB << " MB";
        return;
    }
    const int64_t current = GDALGetCacheMax64();
    if (current > bytes) {
        GDALSetCacheMax64(bytes);
        LOGD << "GDAL block cache bounded from " << current / MiB << " MB to " << bytes / MiB << " MB";
    }
}

}  // namespace ddb
