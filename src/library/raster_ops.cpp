/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "raster_ops.h"

#include <new>
#include <string>
#include <system_error>

#include "build_budget.h"

namespace ddb {
namespace raster {

namespace {

bool isNoSpace(const std::error_code &code) {
    return code == std::errc::no_space_on_device;
}

}  // namespace

ErrorCode classify(const std::exception &failure) {
    if (const auto *op = dynamic_cast<const RasterOpException *>(&failure))
        return op->code();
    if (dynamic_cast<const std::bad_alloc *>(&failure) != nullptr)
        return ErrorCode::Limit;
    if (const auto *fs = dynamic_cast<const FSException *>(&failure))
        return isNoSpace(fs->code()) ? ErrorCode::NoSpace : ErrorCode::Internal;
    if (const auto *retryable = dynamic_cast<const RetryableBuildException *>(&failure)) {
        return retryable->reason() == RetryableBuildReason::IdleTimeout ? ErrorCode::Timeout : ErrorCode::NoSpace;
    }
    if (const auto *system = dynamic_cast<const std::system_error *>(&failure))
        return isNoSpace(system->code()) ? ErrorCode::NoSpace : ErrorCode::Internal;
    // What GDAL could not read of a file is the file's fault
    if (dynamic_cast<const GDALException *>(&failure) != nullptr)
        return ErrorCode::Unreadable;
    return ErrorCode::Internal;
}

parallel::CpuTokens acquireBuildThreads(const std::optional<int> &cap) {
    BuildOptions options;
    options.threads = cap;
    return parallel::tryAcquireThreads(static_cast<size_t>(BuildResourceBudget::fromConfig(options).threads));
}

BuildThreads::BuildThreads(const std::optional<int> &granted)
    : tokens_(acquireBuildThreads(granted)),
      numThreads_("GDAL_NUM_THREADS", std::to_string(tokens_.threads()).c_str()) {}

}  // namespace raster
}  // namespace ddb
