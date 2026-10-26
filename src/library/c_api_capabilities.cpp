/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "c_api_capabilities.h"

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "c_api_util.h"
#include "ddb.h"
#include "json.h"
#include "version.h"

namespace ddb::capi {

namespace {

// Every exported DDB* function, one string literal per line of src/include/ddb.symbols:
// src/CMakeLists.txt generates the list from the manifest, which stays the only copy
constexpr std::string_view kExportedSymbols[] = {
#include "ddb_symbols.inc"
};

// Every DDBErr code under its enumerator name without the DDBERR_ prefix (the same token, so a
// name cannot drift from its code), in value order
#define DDB_ERROR_CODE(name)              \
    std::pair<std::string_view, DDBErr> { \
        #name, DDBERR_##name              \
    }
constexpr std::array kErrorCodes = {DDB_ERROR_CODE(NONE),
                                    DDB_ERROR_CODE(EXCEPTION),
                                    DDB_ERROR_CODE(BUILDDEPMISSING),
                                    DDB_ERROR_CODE(BUILDINPROGRESS),
                                    DDB_ERROR_CODE(CANCELED),
                                    DDB_ERROR_CODE(BUSY),
                                    DDB_ERROR_CODE(NOTFOUND),
                                    DDB_ERROR_CODE(INVALIDARGS),
                                    DDB_ERROR_CODE(RETRYABLE),
                                    DDB_ERROR_CODE(SHUTDOWN)};
#undef DDB_ERROR_CODE

// The exports whose work checks the caller's cancel token while it runs: their
// steps check cancel::throwIfCanceled() or run through a checkpoint (GDAL progress, raster
// reads, forEachIndex items, SQLite statements, external tools). Sorted, so the capabilities
// list is stable. DDBExportRaster2 also stops when its progress callback asks to.
constexpr std::string_view kCancellable[] = {
    "DDBApplyDelta",
    "DDBApplyDeltaEx",
    "DDBBuild",
    "DDBBuildEx",
    "DDBCalculateVolume",
    "DDBComputeDeltaLocals",
    "DDBDetectAllStockpiles",
    "DDBDetectStockpile",
    "DDBExportRaster",
    "DDBExportRaster2",
    "DDBGenerateContours",
    "DDBGenerateMemoryThumbnail",
    "DDBGenerateMemoryThumbnailEx",
    "DDBGenerateThumbnail",
    "DDBGetRasterAreaStats",
    "DDBGetRasterMetadata",
    "DDBGetRasterPointValue",
    "DDBGetRasterProfile",
    "DDBGetRasterValueInfo",
    "DDBMaskBorders",
    "DDBMemoryTile",
    "DDBMemoryTileEx",
    "DDBMemoryTileFmt",
    "DDBRenderRasterIndex",
    "DDBRenderRasterRegion",
    "DDBRenderRasterRegionEx",
    "DDBTile",
};

static_assert(
    [] {
        for (size_t i = 0; i < kErrorCodes.size(); i++)
            if (static_cast<size_t>(kErrorCodes[i].second) != i)
                return false;
        return true;
    }(),
    "kErrorCodes must list the DDBErr codes in value order, without gaps");
static_assert(std::ranges::all_of(kCancellable,
                                  [](std::string_view name) {
                                      return std::ranges::find(kExportedSymbols, name) !=
                                             std::end(kExportedSymbols);
                                  }),
              "a cancellable function must be an export listed in ddb.symbols");
static_assert(std::ranges::adjacent_find(kCancellable, std::ranges::greater_equal{}) ==
                  std::end(kCancellable),
              "kCancellable must be sorted, without duplicates");

json stringArray(std::span<const std::string_view> items) {
    json out = json::array();
    for (const auto item : items)
        out.push_back(item);
    return out;
}

std::string buildCapabilities() {
    json errorCodes = json::object();
    for (const auto& [name, code] : kErrorCodes)
        errorCodes[std::string(name)] = static_cast<int>(code);
    json errorReasons = json::array();
    for (const auto& reason : kRetryableReasons)
        errorReasons.push_back(reason.second);
    return json{{"abi", {{"major", DDB_ABI_MAJOR}, {"minor", DDB_ABI_MINOR}}},
                {"version", ddb::version()},
                {"errorCodes", std::move(errorCodes)},
                {"errorReasons", std::move(errorReasons)},
                {"cancellable", stringArray(kCancellable)},
                {"symbols", stringArray(kExportedSymbols)}}
        .dump();
}

}  // namespace

const char* capabilities() noexcept {
    try {
        // Built on the first call; C++ guarantees one thread builds it while the others wait
        static const std::string capabilities = buildCapabilities();
        return capabilities.c_str();
    } catch (...) {
        // Out of memory while building: no exception may cross the C boundary, and the
        // next call builds it again
        return nullptr;
    }
}

}  // namespace ddb::capi
