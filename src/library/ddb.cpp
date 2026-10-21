/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "ddb.h"

#include <cpr/cpr.h>

#include <cmath>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <random>
#include <type_traits>
#include <algorithm>
#include <array>
#include <initializer_list>
#include <span>
#include <string_view>
#include <utility>

#include <sstream>

#ifdef WIN32
#include <windows.h>
#endif

#include "build.h"
#include "build_budget.h"
#include "build_manifest.h"
#include "c_api_util.h"
#include "cancellation.h"
#include "log_forwarding.h"
#include "database.h"
#include "databasepool.h"
#include "raster_cache.h"
#include "dbops.h"
#include "cad.h"
#include "delta.h"
#include "entry.h"
#include "entry_path.h"
#include "entry_types.h"
#include "exceptions.h"
#include "gdal_raii.h"
#include "hash.h"
#include "info.h"
#include "json.h"
#include "logger.h"
#include "mask.h"
#include "align.h"
#include "merge_multispectral.h"
#include "mio.h"
#include "parallel.h"
#include "passwordmanager.h"
#include "process_config.h"
#include "process_init.h"
#include "raster_region.h"
#include "raster_utils.h"
#include "vector_query.h"
#include "sensorprofile.h"
#include "stac.h"
#include "status.h"
#include "syncmanager.h"
#include "tagmanager.h"
#include "thermal.h"
#include "raster_analysis.h"
#include "raster_profile.h"
#include "contour.h"
#include "volume.h"
#include "stockpile.h"
#include "thumbs.h"
#include "tilerhelper.h"
#include "utils.h"
#include "vegetation.h"
#include "version.h"

using namespace ddb;
// Every export is one call of the mapper (c_api_util.h): guarded, guardedOutput or guardedBuffer.
// The template argument names the codes besides NONE and EXCEPTION it may return: none means
// kFrozenCodes (ABI 1.0 exports), kAbi11Codes is for the exports added in ABI 1.1.
using capi::codeBit;
using capi::guarded;
using capi::guardedBuffer;
using capi::guardedOutput;
using capi::kAbi11Codes;
using capi::kFrozenCodes;
using capi::optStr;
using capi::parseJsonArg;
using capi::pathArg;
using capi::requireDdbPath;
using capi::requireOptions;
using capi::requireOutput;
using capi::requirePathList;
using capi::requireText;

DDBErr DDBRegisterProcess(bool verbose) {
    return capi::initializeProcess(&ddb::registerProcess, verbose);
}

// ------------------------------------------------------------------ exempt accessors
// They never read or change the last error, and work in any state of the library

const char* DDBGetVersion() {
    return ddb::version();
}

const char* DDBGetLastError() {
    return capi::lastErrorMessage();
}

const char* DDBGetLastErrorReason() {
    return capi::lastErrorReason();
}

DDBErr DDBVSIFree(uint8_t* buffer) {
    // nullptr is allowed - matches C free() semantics (no-op)
    if (buffer != nullptr)
        VSIFree(buffer);
    return DDBERR_NONE;
}

DDBErr DDBFree(char* ptr) {
    // ptr == nullptr is allowed (no-op, matches standard free behaviour)
    free(ptr);
    return DDBERR_NONE;
}

// Deprecated: the library sets and clears the last error itself
void DDBSetLastError(const char* err) {
    if (err == nullptr)
        capi::clearLastError();
    else
        capi::setLastError(err);
}

namespace {

// Every exported DDB* function, one string literal per line of src/include/ddb.symbols:
// src/CMakeLists.txt generates the list from the manifest, which stays the only copy (XI-01)
constexpr std::string_view kExportedSymbols[] = {
#include "ddb_symbols.inc"
};

// Every DDBErr code under its enumerator name without the DDBERR_ prefix (the same token, so a
// name cannot drift from its code), in value order
#define DDB_ERROR_CODE(name) std::pair<std::string_view, DDBErr>{#name, DDBERR_##name}
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

// The exports whose work checks the caller's cancel token while it runs (DQ-15, XI-07): their
// steps check cancel::throwIfCanceled() or run through a checkpoint (GDAL progress, raster
// reads, forEachIndex items, SQLite statements, external tools). Sorted, so the capabilities
// list is stable. Rollout: the render paths, then DDBBuild; raster analysis and DDBApplyDelta(Ex)
// come later. DDBExportRaster2 also stops when its progress callback asks to.
constexpr std::string_view kCancellable[] = {
    "DDBBuild",
    "DDBExportRaster",
    "DDBExportRaster2",
    "DDBGenerateMemoryThumbnail",
    "DDBGenerateMemoryThumbnailEx",
    "DDBGenerateThumbnail",
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
static_assert(std::ranges::adjacent_find(kCancellable, std::ranges::greater_equal{}) == std::end(kCancellable),
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
    for (const auto& reason : capi::kRetryableReasons)
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

uint32_t DDBGetAbiVersion() {
    return (static_cast<uint32_t>(DDB_ABI_MAJOR) << 16) | static_cast<uint32_t>(DDB_ABI_MINOR);
}

const char* DDBGetCapabilities() {
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

// ------------------------------------------------------------------ process configuration

DDBErr DDBConfigure(const char* configJson) {
    return guarded<kAbi11Codes>([&] { ProcessConfig::get().configure(parseJsonArg(configJson)); });
}

DDBErr DDBGetEffectiveConfig(char** output) {
    return guardedOutput<kAbi11Codes>(output, [&] {
        requireOutput(output);
        return ProcessConfig::get().effective();
    });
}

// ------------------------------------------------------------------ shutdown (XI-07)

namespace {

// What DDBShutdown closes once no call is in flight: what the process keeps open between calls
void closeKeptResources() {
    const size_t connections = DatabasePool::instance().shutdown();
    const size_t rasters = RasterCache::instance().clear();
    LOGD << "Shutdown: closed " << connections << " pooled index connections and " << rasters
         << " cached rasters";
}

}  // namespace

DDBErr DDBShutdown(int timeoutMs) {
    return capi::shutdown(timeoutMs, &closeKeptResources);
}

// ------------------------------------------------------------------ cancellation (XI-07)

// DDBCancelToken is opaque to the caller: the library's cancel::Token
static cancel::Token* tokenOf(DDBCancelToken* token) noexcept {
    return reinterpret_cast<cancel::Token*>(token);
}

DDBErr DDBCreateCancelToken(DDBCancelToken** token) {
    return guardedOutput<kAbi11Codes>(token, [&] {
        requireOutput(token);
        return reinterpret_cast<DDBCancelToken*>(cancel::Token::create());
    });
}

DDBErr DDBCancel(DDBCancelToken* token) {
    // A host cancels its requests while it shuts down: allowed in any state
    return guarded<kAbi11Codes, capi::CallKind::Unrestricted>([&] {
        requireOutput(token, "No cancel token provided");
        tokenOf(token)->cancel(cancel::kCanceledByToken);
    });
}

// Exempt like DDBFree: works in any state and never touches the last error
DDBErr DDBFreeCancelToken(DDBCancelToken* token) {
    if (token != nullptr)
        tokenOf(token)->release();
    return DDBERR_NONE;
}

DDBErr DDBSetThreadCancelToken(DDBCancelToken* token) {
    return guarded<kAbi11Codes, capi::CallKind::Setting>([&] { cancel::setThreadToken(tokenOf(token)); });
}

// ------------------------------------------------------------------ logging (XI-08)

static_assert(DDB_LOG_TRACE == logfwd::kTrace && DDB_LOG_DEBUG == logfwd::kDebug && DDB_LOG_INFO == logfwd::kInfo &&
                  DDB_LOG_WARNING == logfwd::kWarning && DDB_LOG_ERROR == logfwd::kError &&
                  DDB_LOG_FATAL == logfwd::kFatal,
              "DDBLogLevel and the levels of log_forwarding.h are the same");
static_assert(std::is_same_v<DDBLogCallback, logfwd::Callback>, "DDBLogCallback is log_forwarding's callback");

DDBErr DDBSetLogCallback(DDBLogCallback callback, int minLevel, void* userData) {
    return guarded<kAbi11Codes, capi::CallKind::Setting>([&] { logfwd::setCallback(callback, minLevel, userData); });
}

DDBErr DDBSetThreadOperation(uint64_t opId) {
    return guarded<kAbi11Codes, capi::CallKind::Setting>([&] { logfwd::setThreadOperation(opId); });
}

// ------------------------------------------------------------------ files and index

DDBErr DDBNormalizePath(const char* path, const char* reservedJson, char** output) {
    return guardedOutput<kAbi11Codes>(output, [&] {
        if (path == nullptr)
            throw InvalidArgsException("No path provided");
        requireOutput(output);

        const auto rules = ReservedRules::fromJson(reservedJson);
        return normalizeEntryPath(path, rules).toJson();
    });
}

DDBErr DDBInit(const char* directory, char** outPath) {
    return guardedOutput(outPath, [&] {
        if (directory == nullptr)
            throw InvalidArgsException("No directory provided");
        requireOutput(outPath, "No output provided");

        return initIndex(directory);
    });
}

DDBErr DDBAdd(const char* ddbPath,
              const char** paths,
              int numPaths,
              char** output,
              bool recursive) {
    return guardedOutput(output, [&] {
        requireDdbPath(ddbPath);
        const auto pathList = requirePathList(paths, numPaths);
        requireOutput(output);

        const auto db = ddb::open(std::string(ddbPath), true);
        auto outJson = json::array();
        addToIndex(db.get(),
                   ddb::expandPathList(pathList, recursive, 0),
                   [&outJson](const Entry& e, bool) {
                       json j;
                       e.toJSON(j);
                       outJson.push_back(j);
                       return true;
                   });
        return outJson;
    });
}

// Records a path that cannot be expanded as an item error, relative to the dataset like the
// entries/unchanged buckets: Registry C# matching keys on relative paths, a caller-supplied
// absolute path would never match the byPath lookup. Unclassified failures of the path
// expansion (e.g. std::filesystem_error) are filesystem errors.
static void addPathError(Database *db, const std::string &p, const std::exception &ex, AddResult &result) {
    const char *code = addErrorCode(ex);
    if (code == nullptr) code = "FS";
    const auto relPath = io::Path(p).relativeTo(db->rootDirectory()).generic();
    result.errors.push_back({relPath, code, ex.what()});
}

static AddOptions toAddOptions(const DDBAddOptions *options) {
    AddOptions addOpts;
    addOpts.stopOnError = options->stopOnError;
    addOpts.maxConflictRetries = options->maxConflictRetries;
    return addOpts;
}

static void validateAddArgs(const char *ddbPath, const DDBAddOptions *options, char **output) {
    requireDdbPath(ddbPath);
    requireOutput(options, "Options pointer is null");
    requireOutput(output);
}

// { "entries": [...], "unchanged": [...], "errors": [...] }
static json addResultJson(const AddResult &result) {
    auto outJson = json::object();
    outJson["entries"] = json::array();
    for (auto& [entry, isUpdate] : result.entries) {
        json j;
        entry.toJSON(j);
        j["status"] = isUpdate ? "updated" : "added";
        outJson["entries"].push_back(j);
    }

    outJson["unchanged"] = json::array();
    for (auto& path : result.unchanged)
        outJson["unchanged"].push_back(json{{"path", path}});

    outJson["errors"] = json::array();
    for (auto& err : result.errors)
        outJson["errors"].push_back(json{{"path", err.path}, {"code", err.code}, {"message", err.message}});

    return outJson;
}

DDBErr DDBAddWithOptions(const char* ddbPath,
                         const char** paths,
                         int numPaths,
                         const DDBAddOptions* options,
                         char** output) {
    return guardedOutput(output, [&] {
        const auto pathList = requirePathList(paths, numPaths);
        validateAddArgs(ddbPath, options, output);

        const auto db = ddb::open(std::string(ddbPath), true);
        const auto addOpts = toAddOptions(options);

        AddResult result;
        std::vector<std::string> expandedPaths;
        // Expand paths per-item when stopOnError is false: expandPathList throws for a missing
        // path, which would abort the entire batch and bypass per-item error capture.
        if (addOpts.stopOnError) {
            expandedPaths = ddb::expandPathList(pathList, options->recursive, 0);
        } else {
            for (const auto &p : pathList) {
                try {
                    auto partial = ddb::expandPathList({p}, options->recursive, 0);
                    expandedPaths.insert(expandedPaths.end(), partial.begin(), partial.end());
                } catch (const std::exception &ex) {
                    addPathError(db.get(), p, ex, result);
                }
            }
        }

        addToIndexEx(db.get(), expandedPaths, addOpts, result);
        return addResultJson(result);
    });
}

DDBErr DDBAddWithHints(const char *ddbPath, const DDBAddHint *files, int numFiles,
                       const DDBAddOptions *options, char **output) {
    return guardedOutput(output, [&] {
        if (files == nullptr || numFiles <= 0)
            throw InvalidArgsException("No files provided");

        validateAddArgs(ddbPath, options, output);

        const auto db = ddb::open(std::string(ddbPath), true);
        const auto addOpts = toAddOptions(options);

        AddResult result;
        std::vector<KnownFile> known;
        known.reserve(static_cast<size_t>(numFiles));
        for (int i = 0; i < numFiles; i++) {
            const auto &f = files[i];
            if (utils::isNullOrEmptyOrWhitespace(f.path))
                throw InvalidArgsException("File " + std::to_string(i) + " has no path");

            const std::string p(f.path);
            if (!fs::exists(pathArg(f.path))) {
                // A missing file would abort the whole batch in addToIndexEx
                const FSException ex(p + " does not exist");
                if (addOpts.stopOnError) throw ex;
                addPathError(db.get(), p, ex, result);
                continue;
            }

            known.push_back({p, optStr(f.sha256), f.size, f.mtime});
        }

        addToIndexEx(db.get(), known, addOpts, result);
        return addResultJson(result);
    });
}

DDBErr DDBRemove(const char* ddbPath, const char** paths, int numPaths) {
    return guarded([&] {
        requireDdbPath(ddbPath);
        const auto pathList = requirePathList(paths, numPaths);

        const auto db = ddb::open(std::string(ddbPath), true);
        removeFromIndex(db.get(), pathList);
    });
}

DDBErr DDBInfo(const char** paths,
               int numPaths,
               char** output,
               const char* format,
               bool recursive,
               int maxRecursionDepth,
               const char* geometry,
               bool withHash,
               bool stopOnError) {
    return guardedOutput(output, [&] {
        if (!utils::isValidNonEmptyStringParam(format))
            throw InvalidArgsException("No format provided");

        if (!utils::isValidNonEmptyStringParam(geometry))
            throw InvalidArgsException("No geometry provided");

        const auto input = requirePathList(paths, numPaths);
        requireOutput(output);

        if (maxRecursionDepth < 0)
            throw InvalidArgsException("Invalid max recursion depth");

        std::ostringstream ss;
        info(input, ss, format, recursive, maxRecursionDepth, geometry, withHash, stopOnError);
        return ss.str();
    });
}

DDBErr DDBReleaseConnections(const char* path, int* closed) {
    return guardedOutput(closed, [&] {
        requireText(path, "No path provided");

        const auto folder = pathArg(path);
        const auto count = ddb::DatabasePool::instance().releaseConnections(folder) +
                           ddb::releaseCachedFiles(folder);
        return static_cast<int>(count);
    });
}

DDBErr DDBCheckpoint(const char* ddbPath, int* complete) {
    return guardedOutput(complete, [&] {
        requireDdbPath(ddbPath);

        return ddb::checkpointIndex(std::string(ddbPath)) ? 1 : 0;
    });
}

DDBErr DDBGet(const char* ddbPath, const char* path, char** output) {
    return guardedOutput<kFrozenCodes | codeBit(DDBERR_NOTFOUND)>(output, [&] {
        requireDdbPath(ddbPath);
        requireText(path, "No path provided");

        const auto db = ddb::open(std::string(ddbPath), false);

        ddb::Entry entry;
        if (!ddb::getEntryWithMeta(db.get(), std::string(path), entry))
            throw EntryNotFoundException("No entry " + std::string(path));
        return entry.toJSONString();
    });
}

DDBErr DDBList(const char* ddbPath,
               const char** paths,
               int numPaths,
               char** output,
               const char* format,
               bool recursive,
               int maxRecursionDepth) {
    return guardedOutput(output, [&] {
        requireDdbPath(ddbPath);

        if (!utils::isValidNonEmptyStringParam(format))
            throw InvalidArgsException("No format provided");

        const auto pathList = requirePathList(paths, numPaths);
        requireOutput(output);

        if (maxRecursionDepth < 0)
            throw InvalidArgsException("Invalid max recursion depth");

        const auto db = ddb::open(std::string(ddbPath), true);

        std::ostringstream ss;
        listIndex(db.get(), pathList, ss, format, recursive, maxRecursionDepth);
        return ss.str();
    });
}

// The query of DDBListEntries and DDBListEntriesEx
static EntryQuery entryQuery(const char* folder, bool recursive, const char* namePattern, const char* types,
                             bool withDetails) {
    EntryQuery query;
    query.folder = optStr(folder);
    query.recursive = recursive;
    query.namePattern = optStr(namePattern);
    query.types = utils::parseEntryTypeList(types, /*allowDirectory=*/true);
    query.withDetails = withDetails;
    return query;
}

DDBErr DDBListEntries(const char* ddbPath, const char* folder, bool recursive, const char* namePattern,
                      const char* types, bool withDetails, char** output) {
    return guardedOutput(output, [&] {
        requireDdbPath(ddbPath);
        requireOutput(output);

        const EntryQuery query = entryQuery(folder, recursive, namePattern, types, withDetails);
        const auto db = ddb::open(std::string(ddbPath), false);
        return listEntriesJson(db.get(), query, false);
    });
}

DDBErr DDBListEntriesEx(const char* ddbPath, const char* folder, const char* namePattern, const char* types,
                        const DDBListEntriesOptions* options, char** output) {
    return guardedOutput<kAbi11Codes>(output, [&] {
        requireDdbPath(ddbPath);
        const auto opts = requireOptions(options, sizeof(DDBListEntriesOptions), "options");
        requireOutput(output);

        const EntryQuery query = entryQuery(folder, opts.recursive, namePattern, types, opts.withDetails);
        const auto db = ddb::open(std::string(ddbPath), false);
        return listEntriesJson(db.get(), query, opts.withBuild);
    });
}

DDBErr DDBSearch(const char* ddbPath, const char* query, char** output, const char* format) {
    return guardedOutput(output, [&] {
        requireDdbPath(ddbPath);

        if (!utils::isValidStringParam(query))
            throw InvalidArgsException("No query provided");

        if (!utils::isValidNonEmptyStringParam(format))
            throw InvalidArgsException("No format provided");

        requireOutput(output);

        const auto db = ddb::open(std::string(ddbPath), false);

        std::ostringstream ss;
        searchIndex(db.get(), query, ss, format);
        return ss.str();
    });
}

DDBErr DDBAppendPassword(const char* ddbPath, const char* password) {
    return guarded([&] {
        requireDdbPath(ddbPath);

        if (!utils::isValidNonEmptyStringParam(password))
            throw InvalidArgsException("No password provided");

        const auto db = ddb::open(std::string(ddbPath), true);
        PasswordManager manager(db.get());
        manager.append(std::string(password));
    });
}

DDBErr DDBVerifyPassword(const char* ddbPath, const char* password, bool* verified) {
    return guardedOutput(verified, [&] {
        requireDdbPath(ddbPath);

        // We allow empty password verification
        if (password == nullptr)
            throw InvalidArgsException("No password provided");

        requireOutput(verified, "Output parameter pointer is null");

        const auto db = ddb::open(std::string(ddbPath), true);
        PasswordManager manager(db.get());
        return manager.verify(std::string(password));
    });
}

DDBErr DDBClearPasswords(const char* ddbPath) {
    return guarded([&] {
        requireDdbPath(ddbPath);

        const auto db = ddb::open(std::string(ddbPath), true);
        PasswordManager manager(db.get());
        manager.clearAll();
    });
}

DDBErr DDBStatus(const char* ddbPath, char** output) {
    return guardedOutput(output, [&] {
        requireDdbPath(ddbPath);
        requireOutput(output, "No output provided");

        const auto db = ddb::open(std::string(ddbPath), true);

        std::ostringstream ss;

        const auto cb = [&ss](ddb::FileStatus status, const std::string&) {
            switch (status) {
                case NotIndexed:
                    ss << "?\t";
                    break;
                case Deleted:
                    ss << "!\t";
                    break;
                case Modified:
                    ss << "M\t";
                    break;
                case NotModified:
                    break;
            }
        };

        statusIndex(db.get(), cb);
        return ss.str();
    });
}

// @deprecated
DDBErr DDBChattr(const char* ddbPath, const char* attrsJson, char** output) {
    return guardedOutput(output, [&] {
        requireDdbPath(ddbPath);

        if (!utils::isValidStringParam(attrsJson))
            throw InvalidArgsException("No attributes JSON provided");

        requireOutput(output);

        LOGD << "Deprecated DDBChattr call: please use DDBMetaSet instead as DDBChattr will be removed "
                "in the near future.";

        const auto db = ddb::open(std::string(ddbPath), true);
        const json j = parseJsonArg(attrsJson);
        for (auto& el : j.items()) {
            if (el.key() == "public" && el.value().is_boolean()) {
                db->getMetaManager()->set("visibility", el.value() ? "1" : "0");
            } else {
                throw InvalidArgsException("Invalid attribute " + el.key());
            }
        }
        return db->getProperties();
    });
}

// ------------------------------------------------------------------ thumbnails and tiles

DDBErr DDBGenerateThumbnail(const char* filePath, int size, const char* destPath) {
    return guarded([&] {
        requireText(filePath, "No file path provided");
        requireText(destPath, "No destination path provided");

        if (size < 0)
            throw InvalidArgsException("Invalid size parameter");

        generateThumb(pathArg(filePath), size, pathArg(destPath), true, nullptr, nullptr);
    });
}

// The checks of an export with a buffer output
static void requireBufferOutput(uint8_t** outBuffer, int* outBufferSize) {
    requireOutput(outBuffer, "Output buffer pointer is null");
    requireOutput(outBufferSize, "Output buffer size pointer is null");
}

// The checks of the tile exports
static void requireTileArgs(int tz, int tx, int ty, int tileSize) {
    if (tileSize < 0)
        throw InvalidArgsException("Invalid tile size parameter");

    if (tz < 0 || tx < 0 || ty < 0)
        throw InvalidArgsException("Invalid tile coordinates");
}

DDBErr DDBGenerateMemoryThumbnail(const char* filePath,
                                  int size,
                                  uint8_t** outBuffer,
                                  int* outBufferSize) {
    return guardedBuffer(outBuffer, outBufferSize, [&](uint8_t** buffer, int* bufferSize) {
        requireText(filePath, "No file path provided");

        if (size < 0)
            throw InvalidArgsException("Invalid size parameter");

        requireBufferOutput(outBuffer, outBufferSize);

        generateThumb(pathArg(filePath), size, "", true, buffer, bufferSize);
    });
}

DDBErr DDBTile(const char* inputPath,
               int tz,
               int tx,
               int ty,
               char** outputTilePath,
               int tileSize,
               bool tms,
               bool forceRecreate) {
    return guardedOutput(outputTilePath, [&] {
        requireText(inputPath, "No input path provided");
        requireOutput(outputTilePath, "Output tile path pointer is null");
        requireTileArgs(tz, tx, ty, tileSize);

        const auto tilePath = ddb::TilerHelper::getFromUserCache(pathArg(inputPath),
                                                                 tz,
                                                                 tx,
                                                                 ty,
                                                                 tileSize,
                                                                 tms,
                                                                 forceRecreate);
        return io::toUtf8(tilePath);
    });
}

DDBErr DDBMemoryTile(const char* inputPath,
                     int tz,
                     int tx,
                     int ty,
                     uint8_t** outBuffer,
                     int* outBufferSize,
                     int tileSize,
                     bool tms,
                     bool forceRecreate,
                     const char* inputPathHash) {
    return guardedBuffer(outBuffer, outBufferSize, [&](uint8_t** buffer, int* bufferSize) {
        requireText(inputPath, "No input path provided");
        requireBufferOutput(outBuffer, outBufferSize);
        requireTileArgs(tz, tx, ty, tileSize);

        // inputPathHash can be null or empty, it's optional
        ddb::TilerHelper::getTile(pathArg(inputPath),
                                  tz,
                                  tx,
                                  ty,
                                  tileSize,
                                  tms,
                                  forceRecreate,
                                  "",
                                  buffer,
                                  bufferSize,
                                  optStr(inputPathHash));
    });
}

DDBErr DDBMemoryTileFmt(const char* inputPath,
                        int tz,
                        int tx,
                        int ty,
                        uint8_t** outBuffer,
                        int* outBufferSize,
                        int tileSize,
                        bool tms,
                        bool forceRecreate,
                        const char* inputPathHash,
                        const char* outputFormat) {
    return guardedBuffer(outBuffer, outBufferSize, [&](uint8_t** buffer, int* bufferSize) {
        requireText(inputPath, "No input path provided");
        requireBufferOutput(outBuffer, outBufferSize);
        requireTileArgs(tz, tx, ty, tileSize);

        const std::string fmtStr =
            (outputFormat && *outputFormat) ? std::string(outputFormat) : std::string("png");

        ddb::TilerHelper::getTile(pathArg(inputPath),
                                  tz,
                                  tx,
                                  ty,
                                  tileSize,
                                  tms,
                                  forceRecreate,
                                  "",
                                  fmtStr,
                                  buffer,
                                  bufferSize,
                                  optStr(inputPathHash));
    });
}

// ------------------------------------------------------------------ delta and sync

DDBErr DDBDelta(const char* ddbSourceStamp,
                const char* ddbTargetStamp,
                char** output,
                const char* format) {
    return guardedOutput(output, [&] {
        if (ddbSourceStamp == nullptr)
            throw InvalidArgsException("No ddb source path provided");

        if (ddbTargetStamp == nullptr)
            throw InvalidArgsException("No ddb path provided");

        if (!utils::isValidNonEmptyStringParam(format))
            throw InvalidArgsException("No format provided");

        requireOutput(output, "No output provided");

        const json source = parseJsonArg(ddbSourceStamp);
        const json dest = parseJsonArg(ddbTargetStamp);

        std::ostringstream ss;
        delta(source, dest, ss, format);
        return ss.str();
    });
}

// The files a DDBApplyDeltaEx caller verified: a JSON object, delta path -> SHA-256 text
static VerifiedHashes parseVerifiedHashes(const char* verifiedHashesJson) {
    const json j = parseJsonArg(verifiedHashesJson);
    if (!j.is_object())
        throw InvalidArgsException("verifiedHashes must be a JSON object (path: SHA-256)");
    VerifiedHashes hashes;
    for (const auto& [path, hash] : j.items()) {
        if (!hash.is_string())
            throw InvalidArgsException("The verified hash of " + path + " must be a string");
        hashes.emplace(path, hash.get<std::string>());
    }
    return hashes;
}

// DDBApplyDelta and DDBApplyDeltaEx: the conflicts (JSON array of paths). verifiedHashesJson
// NULL: every added file is hashed unless DDB_VERIFY_PUSH_HASHES=0; otherwise the files it
// lists are trusted (once applyDelta checked them against the delta) and the others are
// hashed, whatever the environment
static json applyDeltaConflicts(const char* delta,
                                const char* sourcePath,
                                const char* ddbPath,
                                int mergeStrategy,
                                const char* sourceMetaDump,
                                const char* verifiedHashesJson,
                                char** conflicts) {
    requireText(delta, "No delta provided");
    requireText(sourcePath, "No source path provided");
    requireText(ddbPath, "No ddb path provided");
    requireText(sourceMetaDump, "No source meta dump provided");
    requireOutput(conflicts, "Conflicts output pointer is null");

    if (mergeStrategy < 0)
        throw InvalidArgsException("Invalid merge strategy");

    const Delta d = parseJsonArg(delta);
    const json metaDump = parseJsonArg(sourceMetaDump);

    // The added files must match the hashes of the delta. A host that checked the uploaded
    // bytes itself says which (verifiedHashes); without it DDB_VERIFY_PUSH_HASHES=0 skips the
    // check of every file
    const bool explicitHashes = verifiedHashesJson != nullptr;
    const VerifiedHashes verified = explicitHashes ? parseVerifiedHashes(verifiedHashesJson) : VerifiedHashes{};
    const bool verifyHashes = explicitHashes || [] {
        const char* env = std::getenv("DDB_VERIFY_PUSH_HASHES");
        return env == nullptr || (env[0] != '0' && env[0] != '\0');
    }();

    const auto ddb = ddb::open(std::string(ddbPath), false);
    std::stringstream ss;
    const auto adc = applyDelta(d,
                                pathArg(sourcePath),
                                ddb.get(),
                                static_cast<MergeStrategy>(mergeStrategy),
                                metaDump,
                                ss,
                                verifyHashes,
                                verified);

    json j = json::array();
    for (auto& c : adc)
        j.push_back(c.path);
    return j;
}

DDBErr DDBApplyDelta(const char* delta,
                     const char* sourcePath,
                     const char* ddbPath,
                     int mergeStrategy,
                     const char* sourceMetaDump,
                     char** conflicts) {
    return guardedOutput(conflicts, [&] {
        return applyDeltaConflicts(delta, sourcePath, ddbPath, mergeStrategy, sourceMetaDump, nullptr, conflicts);
    });
}

DDBErr DDBApplyDeltaEx(const char* delta,
                       const char* sourcePath,
                       const char* ddbPath,
                       int mergeStrategy,
                       const char* sourceMetaDump,
                       const char* verifiedHashesJson,
                       char** conflicts) {
    return guardedOutput<kAbi11Codes>(conflicts, [&] {
        return applyDeltaConflicts(delta, sourcePath, ddbPath, mergeStrategy, sourceMetaDump, verifiedHashesJson,
                                   conflicts);
    });
}

DDBErr DDBComputeDeltaLocals(const char* delta,
                             const char* ddbPath,
                             const char* hlDestFolder,
                             char** output) {
    return guardedOutput(output, [&] {
        requireText(delta, "No delta provided");
        requireDdbPath(ddbPath);
        requireOutput(output);

        // hlDestFolder can be empty string, but not null
        if (hlDestFolder == nullptr)
            throw InvalidArgsException("Destination folder parameter is null");

        const Delta d = parseJsonArg(delta);
        const auto ddb = ddb::open(std::string(ddbPath), false);

        auto cdl = computeDeltaLocals(d, ddb.get(), std::string(hlDestFolder));
        json j = json::object();
        for (auto& el : cdl)
            j[el.first] = el.second;
        return j;
    });
}

DDBErr DDBSetTag(const char* ddbPath, const char* newTag) {
    return guarded([&] {
        requireText(ddbPath, "No ddb path provided");

        if (!utils::isValidStringParam(newTag))
            throw InvalidArgsException("No tag provided");

        const auto ddb = ddb::open(std::string(ddbPath), true);
        TagManager manager(ddb.get());
        manager.setTag(newTag);
    });
}

DDBErr DDBGetTag(const char* ddbPath, char** outTag) {
    return guardedOutput(outTag, [&] {
        requireDdbPath(ddbPath);
        requireOutput(outTag, "Output tag pointer is null");

        const auto ddb = ddb::open(std::string(ddbPath), true);
        TagManager manager(ddb.get());
        return manager.getTag();
    });
}

DDBErr DDBGetStamp(const char* ddbPath, char** output) {
    return guardedOutput(output, [&] {
        requireDdbPath(ddbPath);
        requireOutput(output, "No output provided");

        const auto ddb = ddb::open(std::string(ddbPath), true);
        return ddb->getStampJson();
    });
}

DDBErr DDBGetStampChecksum(const char* ddbPath, char** output) {
    return guardedOutput(output, [&] {
        requireDdbPath(ddbPath);
        requireOutput(output, "No output provided");

        const auto ddb = ddb::open(std::string(ddbPath), true);
        return ddb->getStampChecksum();
    });
}

DDBErr DDBRescan(const char* ddbPath, char** output, const char* types, bool stopOnError) {
    return DDBRescanEx(ddbPath, output, types, "", stopOnError);
}

DDBErr DDBRescanEx(const char* ddbPath, char** output, const char* types, const char* extensions,
                   bool stopOnError) {
    return guardedOutput(output, [&] {
        requireDdbPath(ddbPath);
        requireOutput(output);

        // Parse filter strings into vectors
        const std::vector<EntryType> typeFilter = utils::parseEntryTypeList(types);
        const std::vector<std::string> extensionFilter = utils::parseExtensionList(extensions);

        const auto db = ddb::open(std::string(ddbPath), true);
        auto outJson = json::array();

        rescanIndex(db.get(), typeFilter, extensionFilter, stopOnError,
                    [&outJson](const Entry& e, bool success, const std::string& error) {
                        json j;
                        j["path"] = e.path;
                        j["success"] = success;
                        if (!success)
                            j["error"] = error;
                        if (success)
                            e.toJSON(j);
                        outJson.push_back(j);
                        return true; // continue processing
                    });
        return outJson;
    });
}

DDBErr DDBMoveEntry(const char* ddbPath, const char* source, const char* dest) {
    return guarded([&] {
        requireDdbPath(ddbPath);
        requireText(source, "No source path provided");
        requireText(dest, "No dest path provided");

        const auto ddb = ddb::open(std::string(ddbPath), true);
        moveEntry(ddb.get(), std::string(source), std::string(dest));
    });
}

// ------------------------------------------------------------------ builds

DDBErr DDBBuild(const char* ddbPath, const char* source, const char* dest, bool force, bool pendingOnly) {
    // The two build codes DDBBuild has always returned (ABI 1.0)
    constexpr auto kBuildCodes =
        kFrozenCodes | codeBit(DDBERR_BUILDDEPMISSING) | codeBit(DDBERR_BUILDINPROGRESS);
    return guarded<kBuildCodes>([&] {
        requireText(ddbPath, "No ddb path provided");

        const auto ddb = ddb::open(std::string(ddbPath), true);

        // dest and source can be null (optional parameters)
        const auto destPath = optStr(dest);
        const auto path = optStr(source);

        if (path.empty()) {
            if (pendingOnly)
                buildPending(ddb.get(), destPath, force);
            else
                buildAll(ddb.get(), destPath, force);
        } else {
            build(ddb.get(), path, destPath, force);
        }
    });
}

DDBErr DDBCleanup(const char* ddbPath, char** output) {
    return guardedOutput(output, [&] {
        requireText(ddbPath, "No ddb path provided");
        requireOutput(output);

        const auto ddb = ddb::open(std::string(ddbPath), true);
        const auto result = cleanupBuild(ddb.get(), "");

        json j;
        j["entries"] = result.removedEntries;
        j["builds"] = result.removedBuilds;
        return j;
    });
}

DDBErr DDBIsBuildable(const char* ddbPath, const char* path, bool* isBuildable) {
    return guardedOutput(isBuildable, [&] {
        requireDdbPath(ddbPath);

        if (!utils::isValidStringParam(path))
            throw InvalidArgsException("No path provided");

        requireOutput(isBuildable, "Buildable parameter is null");

        const auto ddb = ddb::open(std::string(ddbPath), true);
        std::string subfolder;
        return ddb::isBuildable(ddb.get(), std::string(path), subfolder);
    });
}

DDBErr DDBIsBuildPending(const char* ddbPath, bool* isBuildPending) {
    return guardedOutput(isBuildPending, [&] {
        requireDdbPath(ddbPath);
        requireOutput(isBuildPending, "isBuildPending parameter is null");

        const auto ddb = ddb::open(std::string(ddbPath), true);
        return ddb::isBuildPending(ddb.get());
    });
}

DDBErr DDBGetPendingBuildInfo(const char* ddbPath, char** output) {
    return guardedOutput(output, [&] {
        requireDdbPath(ddbPath);
        requireOutput(output);

        const auto db = ddb::open(std::string(ddbPath), true);
        const auto pending = ddb::getPendingBuildInfo(db.get());

        auto outJson = json::array();
        for (const auto& p : pending) {
            json j;
            j["hash"] = p.hash;
            j["path"] = p.path;
            j["missingDependencies"] = p.missingDependencies;
            j["lastAttempt"] = p.lastAttempt;
            outJson.push_back(j);
        }
        return outJson;
    });
}

DDBErr DDBIsBuildActive(const char* ddbPath, const char* path, bool* isBuildActive) {
    return guardedOutput(isBuildActive, [&] {
        requireDdbPath(ddbPath);
        requireText(path, "No path provided");
        requireOutput(isBuildActive, "isBuildActive parameter is null");

        const auto ddb = ddb::open(std::string(ddbPath), true);
        return ddb::isBuildActive(ddb.get(), std::string(path));
    });
}

DDBErr DDBIsBuildComplete(const char* ddbPath, const char* path, bool* isBuildComplete) {
    return guardedOutput(isBuildComplete, [&] {
        requireDdbPath(ddbPath);
        requireText(path, "No path provided");
        requireOutput(isBuildComplete, "isBuildComplete parameter is null");

        const auto ddb = ddb::open(std::string(ddbPath), true);
        return ddb::isBuildComplete(ddb.get(), std::string(path));
    });
}

DDBErr DDBGetBuildStatus(const char* ddbPath, const char** paths, int numPaths, char** output) {
    return guardedOutput(output, [&] {
        requireDdbPath(ddbPath);
        const auto pathList = capi::toStringList(paths, numPaths);
        requireOutput(output);

        const auto ddb = ddb::open(std::string(ddbPath), true);
        const auto statuses = ddb::getBuildStatus(ddb.get(), pathList);

        json j = json::array();
        for (const auto& s : statuses)
            j.push_back({{"path", s.path}, {"exists", s.exists}, {"buildable", s.buildable}, {"complete", s.complete}});
        return j;
    });
}

// ------------------------------------------------------------------ build manifest (XI-04)

DDBErr DDBGetEntryTypes(char** output) {
    return guardedOutput<kAbi11Codes>(output, [&] {
        requireOutput(output);
        return entryTypesJson();
    });
}

DDBErr DDBGetBuildArtifacts(const char* ddbPath, const char** paths, int numPaths, char** output) {
    return guardedOutput<kAbi11Codes>(output, [&] {
        requireDdbPath(ddbPath);
        const auto pathList = capi::toStringList(paths, numPaths);
        requireOutput(output);

        const auto db = ddb::open(std::string(ddbPath), false);
        return getBuildArtifactsJson(db.get(), pathList);
    });
}

DDBErr DDBResolveBuildFile(const char* ddbPath, const char* hash, const char* path, char** outPath) {
    return guardedOutput<kAbi11Codes>(outPath, [&] {
        requireDdbPath(ddbPath);
        requireText(hash, "No hash provided");
        requireText(path, "No path provided");
        requireOutput(outPath);

        const auto db = ddb::open(std::string(ddbPath), false);
        return io::toUtf8(resolveBuildFile(db.get(), hash, path));
    });
}

DDBErr DDBCopyBuildArtifacts(const char* sourceDdbPath, const char* destinationDdbPath, const char* hash,
                             char** output) {
    return guardedOutput<kAbi11Codes>(output, [&] {
        requireText(sourceDdbPath, "No source directory provided");
        requireText(destinationDdbPath, "No destination directory provided");
        requireText(hash, "No hash provided");
        requireOutput(output);

        const auto source = ddb::open(std::string(sourceDdbPath), false);
        const auto destination = ddb::open(std::string(destinationDdbPath), false);
        return copyBuildArtifacts(source.get(), destination.get(), hash);
    });
}

// ------------------------------------------------------------------ metadata

// The checks of the metadata exports: the dataset, then each string argument, then the output
static void requireMetaArgs(const char* ddbPath,
                            std::initializer_list<std::pair<const char*, const char*>> strings,
                            char** output) {
    requireDdbPath(ddbPath);
    for (const auto& [value, message] : strings)
        if (!utils::isValidStringParam(value))
            throw InvalidArgsException(message);
    requireOutput(output);
}

DDBErr DDBMetaAdd(const char* ddbPath,
                  const char* path,
                  const char* key,
                  const char* data,
                  char** output) {
    return guardedOutput(output, [&] {
        requireMetaArgs(ddbPath,
                        {{path, "No path provided"}, {key, "No key provided"}, {data, "No data provided"}},
                        output);

        const auto ddbPathStr = std::string(ddbPath);
        const auto ddb = ddb::open(ddbPathStr, true);
        return ddb->getMetaManager()->add(std::string(key),
                                          std::string(data),
                                          std::string(path),
                                          ddbPathStr);
    });
}

DDBErr DDBMetaSet(const char* ddbPath,
                  const char* path,
                  const char* key,
                  const char* data,
                  char** output) {
    return guardedOutput(output, [&] {
        requireMetaArgs(ddbPath,
                        {{path, "No path provided"}, {key, "No key provided"}, {data, "No data provided"}},
                        output);

        const auto ddbPathStr = std::string(ddbPath);
        const auto ddb = ddb::open(ddbPathStr, true);
        return ddb->getMetaManager()->set(std::string(key),
                                          std::string(data),
                                          std::string(path),
                                          ddbPathStr);
    });
}

DDBErr DDBMetaRemove(const char* ddbPath, const char* id, char** output) {
    return guardedOutput(output, [&] {
        requireMetaArgs(ddbPath, {{id, "No id provided"}}, output);

        const auto ddb = ddb::open(std::string(ddbPath), true);
        return ddb->getMetaManager()->remove(std::string(id));
    });
}

DDBErr DDBMetaGet(const char* ddbPath, const char* path, const char* key, char** output) {
    return guardedOutput(output, [&] {
        requireMetaArgs(ddbPath, {{path, "No path provided"}, {key, "No key provided"}}, output);

        const auto ddbPathStr = std::string(ddbPath);
        const auto ddb = ddb::open(ddbPathStr, true);
        return ddb->getMetaManager()->get(std::string(key), std::string(path), ddbPathStr);
    });
}

DDBErr DDBMetaUnset(const char* ddbPath, const char* path, const char* key, char** output) {
    return guardedOutput(output, [&] {
        requireMetaArgs(ddbPath, {{path, "No path provided"}, {key, "No key provided"}}, output);

        const auto ddb = ddb::open(std::string(ddbPath), true);
        return ddb->getMetaManager()->unset(std::string(key), std::string(path), std::string(ddbPath));
    });
}

DDBErr DDBMetaList(const char* ddbPath, const char* path, char** output) {
    return guardedOutput(output, [&] {
        requireMetaArgs(ddbPath, {{path, "No path provided"}}, output);

        const auto ddbPathStr = std::string(ddbPath);
        const auto ddb = ddb::open(ddbPathStr, true);
        return ddb->getMetaManager()->list(std::string(path), ddbPathStr);
    });
}

DDBErr DDBMetaDump(const char* ddbPath, const char* ids, char** output) {
    return guardedOutput(output, [&] {
        requireMetaArgs(ddbPath, {{ids, "No ids provided"}}, output);

        const json jIds = parseJsonArg(ids);
        const auto ddb = ddb::open(std::string(ddbPath), true);
        return ddb->getMetaManager()->dump(jIds);
    });
}

DDBErr DDBMetaRestore(const char* ddbPath, const char* dump, char** output) {
    return guardedOutput(output, [&] {
        requireMetaArgs(ddbPath, {{dump, "No dump provided"}}, output);

        const json jDump = parseJsonArg(dump);
        const auto ddb = ddb::open(std::string(ddbPath), true);
        return ddb->getMetaManager()->restore(jDump);
    });
}

// ------------------------------------------------------------------ STAC

namespace {

// A STAC API bbox "minX,minY,maxX,maxY" (EPSG:4326): empty when absent; throws on an invalid one
std::vector<double> parseStacBbox(const char* bbox) {
    std::vector<double> bboxVec;
    if (bbox != nullptr && !utils::isNullOrEmptyOrWhitespace(bbox)) {
        std::stringstream ss(bbox);
        std::string token;
        while (std::getline(ss, token, ',')) {
            try {
                bboxVec.push_back(std::stod(token));
            } catch (...) {
                throw InvalidArgsException("Invalid bbox value: " + token);
            }
        }
        if (bboxVec.size() != 4)
            throw InvalidArgsException("bbox must contain exactly 4 comma-separated values");
    }
    return bboxVec;
}

// A STAC API datetime: a single instant or an interval "start/end" ("../end" or "start/.." for
// open ends); an empty bound is open
struct StacDatetimeRange {
    std::string start;
    std::string end;
};

StacDatetimeRange parseStacDatetime(const char* datetime) {
    StacDatetimeRange range;
    if (datetime != nullptr && !utils::isNullOrEmptyOrWhitespace(datetime)) {
        const std::string dt(datetime);
        const auto slash = dt.find('/');
        if (slash == std::string::npos) {
            range.start = dt;
            range.end = dt;
        } else {
            const std::string lo = dt.substr(0, slash);
            const std::string hi = dt.substr(slash + 1);
            if (lo != "..")
                range.start = lo;
            if (hi != "..")
                range.end = hi;
        }
    }
    return range;
}

}  // namespace

DDBErr DDBStac(const char* ddbPath,
               const char* entry,
               const char* stacCollectionRoot,
               const char* id,
               const char* stacCatalogRoot,
               char** output) {
    return guardedOutput(output, [&] {
        requireDdbPath(ddbPath);
        requireOutput(output);

        // entry, stacCollectionRoot, id, stacCatalogRoot can be null/empty - they are optional
        // parameters
        const auto ddb = ddb::open(std::string(ddbPath), false);
        return ddb::generateStac(std::string(ddbPath),
                                 optStr(entry),
                                 optStr(stacCollectionRoot),
                                 optStr(id),
                                 optStr(stacCatalogRoot));
    });
}

DDBErr DDBStacItemCollection(const char* ddbPath,
                             const char* stacCollectionRoot,
                             const char* id,
                             const char* stacCatalogRoot,
                             const char* bbox,
                             const char* datetime,
                             int limit,
                             int offset,
                             char** output) {
    return guardedOutput(output, [&] {
        requireDdbPath(ddbPath);
        requireOutput(output);

        const auto bboxVec = parseStacBbox(bbox);
        const auto datetimeRange = parseStacDatetime(datetime);

        return ddb::generateStacItemCollection(std::string(ddbPath),
                                               optStr(stacCollectionRoot),
                                               optStr(id),
                                               optStr(stacCatalogRoot),
                                               bboxVec,
                                               datetimeRange.start,
                                               datetimeRange.end,
                                               limit,
                                               offset);
    });
}

DDBErr DDBStacData(const char* ddbPath, const char* entry, char** output) {
    return guardedOutput<kAbi11Codes>(output, [&] {
        requireDdbPath(ddbPath);
        requireOutput(output);

        return ddb::generateStacData(std::string(ddbPath), optStr(entry));
    });
}

DDBErr DDBStacItemCollectionData(const char* ddbPath,
                                 const char* bbox,
                                 const char* datetime,
                                 int limit,
                                 int offset,
                                 char** output) {
    return guardedOutput<kAbi11Codes>(output, [&] {
        requireDdbPath(ddbPath);
        requireOutput(output);

        const auto bboxVec = parseStacBbox(bbox);
        const auto datetimeRange = parseStacDatetime(datetime);
        return ddb::generateStacItemCollectionData(std::string(ddbPath),
                                                   bboxVec,
                                                   datetimeRange.start,
                                                   datetimeRange.end,
                                                   limit,
                                                   offset);
    });
}

// ------------------------------------------------------------------ rasters

DDBErr DDBGetRasterInfo(const char* path, char** output) {
    return guardedOutput(output, [&] {
        requireText(path, "No path provided");
        requireOutput(output);

        auto& spm = ddb::SensorProfileManager::instance();
        std::string jsonStr = spm.getRasterInfoJson(std::string(path));
        return jsonStr;
    });
}

DDBErr DDBGetRasterMetadata(const char* path, const char* formula,
                            const char* bandFilter, char** output) {
    return guardedOutput(output, [&] {
        requireText(path, "No path provided");
        requireOutput(output);

        const std::string formulaStr = optStr(formula);
        const std::string filterStr = optStr(bandFilter);

        // Owned: the formula, statistics and sensor steps below can throw while it is open
        auto dataset = adoptDataset(GDALOpen(path, GA_ReadOnly));
        if (!dataset)
            throw GDALException("Cannot open " + std::string(path));
        GDALDatasetH hDs = datasetHandle(dataset);

        auto& ve = ddb::VegetationEngine::instance();
        json result;

        int nBands = GDALGetRasterCount(hDs);

        // Determine effective band filter
        ddb::BandFilter effectiveFilter;
        if (!filterStr.empty()) {
            effectiveFilter = ddb::VegetationEngine::parseFilter(filterStr, nBands);
        } else {
            effectiveFilter = ve.autoDetectFilter(std::string(path));
        }

        // Statistics
        json statsJson;

        if (!formulaStr.empty()) {
            // Formula statistics: compute on-demand
            const ddb::BandFilter &bf = effectiveFilter;
            int width = GDALGetRasterXSize(hDs);
            int height = GDALGetRasterYSize(hDs);

            // Sample at reduced resolution for performance
            int sampleW = std::min(width, 512);
            int sampleH = std::min(height, 512);
            size_t pixCount = static_cast<size_t>(sampleW) * sampleH;

            std::vector<std::vector<float>> bandDataStorage(nBands);
            std::vector<float*> bandDataPtrs(nBands);

            // Read GDAL nodata values and detect alpha band
            auto nodataInfo = detectBandNodata(hDs, nBands);
            for (int b = 0; b < nBands; b++) {
                bandDataStorage[b].resize(pixCount);
                bandDataPtrs[b] = bandDataStorage[b].data();
                GDALRasterBandH hBand = GDALGetRasterBand(hDs, b + 1);
                GDALRasterIO(hBand, GF_Read, 0, 0, width, height,
                             bandDataPtrs[b], sampleW, sampleH, GDT_Float32, 0, 0);
            }

            // Pre-mask: set pixels to output nodata where alpha=0 or any band
            // matches its GDAL nodata value, so applyFormula excludes them
            float nodata = NODATA_SENTINEL;
            premaskNodata(bandDataPtrs, pixCount, nBands, nodataInfo, nodata);

            std::vector<float> formulaResult(pixCount);
            const auto* formulaObj = ve.getFormula(formulaStr);
            if (!formulaObj)
                throw InvalidArgsException("Unknown formula: " + formulaStr);
            ve.applyFormula(*formulaObj, bf, bandDataPtrs, formulaResult.data(), pixCount, nodata);

            // Compute statistics
            std::vector<double> valid;
            valid.reserve(pixCount);
            double sum = 0, sumSq = 0;
            double fMin = std::numeric_limits<double>::max();
            double fMax = std::numeric_limits<double>::lowest();
            for (size_t i = 0; i < pixCount; i++) {
                if (formulaResult[i] != nodata) {
                    double v = static_cast<double>(formulaResult[i]);
                    valid.push_back(v);
                    sum += v;
                    sumSq += v * v;
                    fMin = std::min(fMin, v);
                    fMax = std::max(fMax, v);
                }
            }

            if (!valid.empty()) {
                double mean = sum / valid.size();
                double variance = (sumSq / valid.size()) - (mean * mean);
                double stddev = std::sqrt(std::max(0.0, variance));

                std::sort(valid.begin(), valid.end());
                double p2 = valid[static_cast<size_t>(valid.size() * 0.02)];
                double p98 = valid[std::min(valid.size() - 1, static_cast<size_t>(valid.size() * 0.98))];

                // Histogram
                int bins = 255;
                std::vector<int> counts(bins, 0);
                std::vector<double> edges(bins + 1);
                double range = fMax - fMin;
                if (range <= 0) range = 1.0;
                for (int i = 0; i <= bins; i++) {
                    edges[i] = fMin + (range * i / bins);
                }
                for (double v : valid) {
                    int idx = static_cast<int>((v - fMin) / range * (bins - 1));
                    idx = std::max(0, std::min(bins - 1, idx));
                    counts[idx]++;
                }

                statsJson["1"] = {
                    {"min", fMin}, {"max", fMax}, {"mean", mean}, {"std", stddev},
                    {"percentiles", {{"p2", p2}, {"p98", p98}}},
                    {"histogram", {{"counts", counts}, {"edges", edges}, {"bins", bins}}}
                };
            }
        } else {
            // Per-band statistics
            for (int i = 1; i <= nBands; i++) {
                GDALRasterBandH hBand = GDALGetRasterBand(hDs, i);
                double bMin, bMax, bMean, bStdDev;
                if (GDALComputeRasterStatistics(hBand, TRUE, &bMin, &bMax, &bMean, &bStdDev, nullptr, nullptr) == CE_None) {
                    statsJson[std::to_string(i)] = {
                        {"min", bMin}, {"max", bMax}, {"mean", bMean}, {"std", bStdDev},
                        {"percentiles", {{"p2", bMean - 2.33 * bStdDev}, {"p98", bMean + 2.33 * bStdDev}}}
                    };
                }
            }
        }

        result["statistics"] = statsJson;

        // Available algorithms for this band filter
        // Include all formulas, mark incompatible ones as disabled
        auto compatibleFormulas = ve.getFormulasForFilter(effectiveFilter);
        auto allFormulas = ve.getAllFormulas();
        json algJson = json::array();
        for (const auto& f : allFormulas) {
            json fj = {
                {"id", f.id}, {"name", f.name}, {"expr", f.expr},
                {"help", f.help}
            };
            if (f.hasRange) {
                fj["range"] = {f.rangeMin, f.rangeMax};
            }
            bool compatible = false;
            for (const auto& cf : compatibleFormulas) {
                if (cf.id == f.id) { compatible = true; break; }
            }
            if (!compatible) {
                fj["disabled"] = true;
                fj["disabledReason"] = "Requires bands: " + f.requiredBands;
            }
            algJson.push_back(fj);
        }
        result["algorithms"] = algJson;

        // Colormaps
        result["colormaps"] = ve.getColormapsJson();

        result["autoBands"] = {{"filter", effectiveFilter.id}, {"match", !effectiveFilter.id.empty()}};

        auto& spm = ddb::SensorProfileManager::instance();
        auto detection = spm.detectSensor(std::string(path));
        result["detectedSensor"] = detection.sensorId;

        dataset.reset();

        return result;
    });
}

namespace {

// The visualization parameters of the Ex thumbnail and tile exports; NULL leaves one unset
ddb::ThumbVisParams visParams(const char* preset, const char* bands, const char* formula,
                              const char* bandFilter, const char* colormap, const char* rescale) {
    ddb::ThumbVisParams params;
    params.preset = optStr(preset);
    params.bands = optStr(bands);
    params.formula = optStr(formula);
    params.bandFilter = optStr(bandFilter);
    params.colormap = optStr(colormap);
    params.rescale = optStr(rescale);
    return params;
}

// The non-NULL elements of a list of input paths (the merge-multispectral exports skip NULL)
std::vector<std::string> nonNullPaths(const char** paths, int numPaths) {
    std::vector<std::string> inputPaths;
    for (int i = 0; i < numPaths; i++) {
        if (paths[i]) inputPaths.emplace_back(paths[i]);
    }
    return inputPaths;
}

}  // namespace


DDBErr DDBGenerateMemoryThumbnailEx(const char* filePath, int size,
                                    const char* preset, const char* bands,
                                    const char* formula, const char* bandFilter,
                                    const char* colormap, const char* rescale,
                                    uint8_t** outBuffer, int* outBufferSize) {
    return guardedBuffer(outBuffer, outBufferSize, [&](uint8_t** buffer, int* bufferSize) {
        requireText(filePath, "No file path provided");
        if (size < 0)
            throw InvalidArgsException("Invalid size parameter");
        requireBufferOutput(outBuffer, outBufferSize);

        ddb::generateImageThumbEx(pathArg(filePath), size, "", buffer, bufferSize,
                                  visParams(preset, bands, formula, bandFilter, colormap, rescale));
    });
}

DDBErr DDBMemoryTileEx(const char* inputPath,
                       int tz, int tx, int ty,
                       int tileSize, bool tms,
                       bool forceRecreate, const char* inputPathHash,
                       const char* preset, const char* bands,
                       const char* formula, const char* bandFilter,
                       const char* colormap, const char* rescale,
                       uint8_t** outBuffer, int* outBufferSize) {
    return guardedBuffer(outBuffer, outBufferSize, [&](uint8_t** buffer, int* bufferSize) {
        requireText(inputPath, "No input path provided");
        requireBufferOutput(outBuffer, outBufferSize);
        requireTileArgs(tz, tx, ty, tileSize);

        // Build vizKey from params for cache differentiation
        std::string vizKey;
        if (preset) vizKey += std::string("p:") + preset;
        if (bands) vizKey += std::string(",b:") + bands;
        if (formula) vizKey += std::string(",f:") + formula;
        if (bandFilter) vizKey += std::string(",bf:") + bandFilter;
        if (colormap) vizKey += std::string(",cm:") + colormap;
        if (rescale) vizKey += std::string(",rs:") + rescale;

        ddb::TilerHelper::getTile(pathArg(inputPath),
                                  tz, tx, ty,
                                  tileSize, tms,
                                  forceRecreate,
                                  "",
                                  visParams(preset, bands, formula, bandFilter, colormap, rescale),
                                  buffer, bufferSize,
                                  optStr(inputPathHash) + vizKey);
    });
}

DDBErr DDBValidateMergeMultispectral(const char** paths, int numPaths, char** output) {
    return guardedOutput(output, [&] {
        if (paths == nullptr || numPaths < 1)
            throw InvalidArgsException("No input paths provided");
        requireOutput(output);

        auto validationResult = ddb::validateMergeMultispectral(nonNullPaths(paths, numPaths));

        json j;
        j["ok"] = validationResult.ok;
        j["errors"] = validationResult.errors;
        j["warnings"] = validationResult.warnings;
        j["summary"] = {
            {"totalBands", validationResult.summary.totalBands},
            {"dataType", validationResult.summary.dataType},
            {"width", validationResult.summary.width},
            {"height", validationResult.summary.height},
            {"crs", validationResult.summary.crs},
            {"pixelSizeX", validationResult.summary.pixelSizeX},
            {"pixelSizeY", validationResult.summary.pixelSizeY},
            {"estimatedSize", validationResult.summary.estimatedSize}
        };

        // Alignment info
        json alignmentJson;
        alignmentJson["detected"] = validationResult.alignment.detected;
        alignmentJson["maxShiftPixels"] = validationResult.alignment.maxShiftPixels;
        alignmentJson["correctionApplied"] = validationResult.alignment.correctionApplied;
        alignmentJson["shiftSource"] = validationResult.alignment.shiftSource;

        json bandsJson = json::array();
        for (size_t i = 0; i < validationResult.alignment.bands.size(); i++) {
            const auto &b = validationResult.alignment.bands[i];
            bandsJson.push_back({
                {"index", i},
                {"name", b.bandName},
                {"wavelength", b.centralWavelength},
                {"shiftX", b.shiftX},
                {"shiftY", b.shiftY}
            });
        }
        alignmentJson["bands"] = bandsJson;
        j["alignment"] = alignmentJson;

        return j;
    });
}

DDBErr DDBPreviewMergeMultispectral(const char** paths, int numPaths,
                                    const char* previewBands, int thumbSize,
                                    uint8_t** outBuffer, int* outBufferSize) {
    return guardedBuffer(outBuffer, outBufferSize, [&](uint8_t** buffer, int* bufferSize) {
        if (paths == nullptr || numPaths < 1)
            throw InvalidArgsException("No input paths provided");
        requireBufferOutput(outBuffer, outBufferSize);

        const auto inputPaths = nonNullPaths(paths, numPaths);

        std::vector<int> bandsVec;
        if (previewBands) {
            std::istringstream ss(previewBands);
            std::string token;
            while (std::getline(ss, token, ',')) {
                try {
                    bandsVec.push_back(std::stoi(token));
                } catch (const std::exception&) {
                    throw InvalidArgsException("Invalid preview band value: '" + token + "'");
                }
            }
        }
        if (bandsVec.size() < 3) {
            bandsVec = {1, 2, 3};
        }

        ddb::previewMergeMultispectral(inputPaths, bandsVec, thumbSize > 0 ? thumbSize : 512, buffer, bufferSize);
    });
}

DDBErr DDBMergeMultispectral(const char** paths, int numPaths, const char* outputPath) {
    return guarded([&] {
        if (paths == nullptr || numPaths < 2)
            throw InvalidArgsException("At least 2 input paths required");
        requireText(outputPath, "No output path provided");

        ddb::mergeMultispectral(nonNullPaths(paths, numPaths), std::string(outputPath));
    });
}

DDBErr DDBValidateAlignRaster(const char* sourcePath,
                              const char* referencePath,
                              char** output) {
    return guardedOutput(output, [&] {
        requireText(sourcePath, "sourcePath is required");
        requireText(referencePath, "referencePath is required");
        requireOutput(output);

        auto r = ddb::validateAlignRaster(std::string(sourcePath),
                                          std::string(referencePath));
        return ddb::alignValidationToJson(r);
    });
}

DDBErr DDBAlignRaster(const char* sourcePath,
                      const char* referencePath,
                      const char* outputPath,
                      const char* mode,
                      char** output) {
    return guardedOutput(output, [&] {
        requireText(sourcePath, "sourcePath is required");
        requireText(referencePath, "referencePath is required");
        requireText(outputPath, "outputPath is required");
        requireOutput(output);

        ddb::AlignOptions opts;
        if (mode) {
            std::string modeStr(mode);
            if (modeStr == "translation")
                opts.mode = ddb::AlignMode::Translation;
            else if (!modeStr.empty() && modeStr != "similarity")
                throw InvalidArgsException("Unknown mode '" + modeStr + "'. Use: similarity | translation");
        }

        auto r = ddb::alignRaster(std::string(sourcePath),
                                  std::string(referencePath),
                                  std::string(outputPath), opts);
        return ddb::alignResultToJson(r);
    });
}

// Build internal, power-of-two overviews on a freshly written GeoTIFF so the
// output supports efficient zoomed/partial reads (closer to a cloud-optimized
// layout). Levels are added while the largest dimension stays >= 256 px, using
// the same DEFLATE+PREDICTOR compression for the pyramids. Best-effort: a
// failure to build overviews is non-fatal (the full-resolution data is intact).
static void buildInternalOverviews(GDALDatasetH hDs, int width, int height) {
    std::vector<int> levels;
    int factor = 2;
    while (std::max(width, height) / factor >= 256 && levels.size() < 8) {
        levels.push_back(factor);
        factor *= 2;
    }
    if (levels.empty()) return;
    // Thread-local config so concurrent exports do not clobber global GDAL state.
    const ScopedThreadConfigOption compress("COMPRESS_OVERVIEW", "DEFLATE");
    const ScopedThreadConfigOption predictor("PREDICTOR_OVERVIEW", "2");
    GDALBuildOverviews(hDs, "AVERAGE", static_cast<int>(levels.size()),
                       levels.data(), 0, nullptr, nullptr, nullptr);
}

// Auto-compute a windowed tile size for the formula export branch.
// - Explicit tileSize > 0: clamped to [64, 8192], then rounded up to the
//   nearest multiple of 16 (GeoTIFF tiling requirement).
// - Auto (tileSize == 0): starts from max(blockSize, 512) clamped to
//   [256, 1024], then halved until the per-tile working set
//   (bandCount input floats + formula result float + RGBA bytes,
//   x2 headroom) fits within ~64 MB, with a floor of 256; the
//   result is then rounded up to the nearest multiple of 16.
static int computeExportTileSize(GDALDatasetH hSrcDs, int bandCount, int tileSize) {
    int result;
    if (tileSize > 0) {
        result = std::clamp(tileSize, 64, 8192);
    } else {
        int bx = 0, by = 0;
        GDALGetBlockSize(GDALGetRasterBand(hSrcDs, 1), &bx, &by);
        int t = std::clamp(std::max(bx, 512), 256, 1024);

        const size_t budgetBytes = 64ull * 1024 * 1024;
        // Working set per tile: bandCount input floats + formula result float +
        // RGBA bytes, with a x2 headroom factor for GDAL internal buffers.
        auto tileBytes = [&](int s) {
            const size_t pix = static_cast<size_t>(s) * s;
            return pix * (static_cast<size_t>(bandCount) * sizeof(float) + sizeof(float) + 4) * 2;
        };
        while (t > 256 && tileBytes(t) > budgetBytes) t /= 2;
        result = std::max(t, 256);
    }

    // GeoTIFF requires tile dimensions to be a multiple of 16. Round up and
    // re-clamp so the value is valid both as a processing window and as the
    // output BLOCKXSIZE/BLOCKYSIZE.
    result = ((result + 15) / 16) * 16;
    return std::clamp(result, 64, 8192);
}

// The output file of an export, deleted unless the export completed (commit): a failed or
// canceled export leaves no partial file
class PartialOutput {
   public:
    explicit PartialOutput(const char* path) : path_(path) {}
    ~PartialOutput() {
        if (!committed_)
            VSIUnlink(path_);
    }
    PartialOutput(const PartialOutput&) = delete;
    PartialOutput& operator=(const PartialOutput&) = delete;
    void commit() noexcept { committed_ = true; }

   private:
    const char* path_;
    bool committed_ = false;
};

// Internal block-windowed raster export shared by DDBExportRaster (legacy ABI)
// and DDBExportRaster2. Throws on errors (mapped to a DDBErr by guarded() in the
// callers), CanceledException when the progress callback asked to stop or the
// call was canceled (DQ-15).
static void exportRasterImpl(const char* inputPath, const char* outputPath,
                            const char* preset, const char* bands,
                            const char* formula, const char* bandFilter,
                            const char* colormap, const char* rescale,
                            int tileSize,
                            DDBProgressCallback progress, void* progressUserData) {
    requireText(inputPath, "No input path provided");
    requireText(outputPath, "No output path provided");
    // Creating the output truncates an existing file, so it must not be the input
    io::assureDistinctOutput(pathArg(inputPath), pathArg(outputPath));

    // One cancellation for the whole call (DQ-15): the token of this call is canceled by the
    // caller's token, by the shutdown, and by the progress callback when it asks to stop
    // (non-zero), so every checkpoint below, GDAL's included, honours all three.
    cancel::CallToken callToken;
    // Reports progress; never throws (GDAL calls it through its progress callback)
    const auto askCallback = [&](double fraction, const char* phase) {
        if (progress != nullptr && progress(fraction, phase, progressUserData) != 0)
            callToken.cancel("Export canceled by the progress callback");
    };
    // Reports progress, then stops the export when it was canceled
    const auto checkpoint = [&](double fraction, const char* phase) {
        askCallback(fraction, phase);
        cancel::throwIfCanceled();
    };

    // Closed on any return or throw
    auto srcGuard = adoptDataset(GDALOpen(inputPath, GA_ReadOnly));
    if (!srcGuard)
        throw GDALException("Cannot open " + std::string(inputPath));
    GDALDatasetH hSrcDs = datasetHandle(srcGuard);

    const int width = GDALGetRasterXSize(hSrcDs);
    const int height = GDALGetRasterYSize(hSrcDs);
    const int bandCount = GDALGetRasterCount(hSrcDs);

    const std::string formulaStr = optStr(formula);
    const std::string bandsStr = optStr(bands);
    const std::string presetStr = optStr(preset);
    const std::string filterStr = optStr(bandFilter);
    const std::string colormapStr = optStr(colormap);
    const std::string rescaleStr = optStr(rescale);

    // Get source geotransform and projection
    double geoTransform[6];
    bool hasGeoTransform = (GDALGetGeoTransform(hSrcDs, geoTransform) == CE_None);
    const char* projRef = GDALGetProjectionRef(hSrcDs);
    std::string projection = projRef ? std::string(projRef) : "";

    if (!formulaStr.empty()) {
        // Formula mode: apply formula + colormap, export as RGBA GeoTIFF.
        // Block-windowed so peak memory is O(tileW * tileH * bandCount) and
        // independent of the full raster size.
        auto& ve = ddb::VegetationEngine::instance();

        ddb::BandFilter bf;
        if (!filterStr.empty()) {
            bf = ddb::VegetationEngine::parseFilter(filterStr, bandCount);
        } else {
            bf = ve.autoDetectFilter(std::string(inputPath));
        }

        const auto* formulaPtr = ve.getFormula(formulaStr);
        if (!formulaPtr)
            throw InvalidArgsException("Unknown formula: " + formulaStr);
        std::string cmId = colormapStr.empty() ? "rdylgn" : colormapStr;
        const auto* cmap = ve.getColormap(cmId);
        if (!cmap)
            throw InvalidArgsException("Unknown colormap: " + cmId);

        const auto nodataInfo = detectBandNodata(hSrcDs, bandCount);
        const float nodata = NODATA_SENTINEL;

        const int tw = computeExportTileSize(hSrcDs, bandCount, tileSize);
        const int th = tw;
        const int ntx = (width + tw - 1) / tw;
        const int nty = (height + th - 1) / th;
        const size_t totalTiles = static_cast<size_t>(ntx) * nty;

        // Reads a window of every band into the supplied per-band buffers and
        // applies the formula, leaving the result in 'result'.
        auto computeWindow = [&](int x0, int y0, int winW, int winH,
                                 std::vector<std::vector<float>>& storage,
                                 std::vector<float*>& ptrs,
                                 std::vector<float>& result) {
            const size_t winPix = static_cast<size_t>(winW) * winH;
            for (int b = 0; b < bandCount; b++) {
                storage[b].resize(winPix);
                ptrs[b] = storage[b].data();
                GDALRasterBandH hBand = GDALGetRasterBand(hSrcDs, b + 1);
                if (GDALRasterIO(hBand, GF_Read, x0, y0, winW, winH,
                                 ptrs[b], winW, winH, GDT_Float32, 0, 0) != CE_None)
                    throw GDALException("Cannot read band " + std::to_string(b + 1));
            }
            premaskNodata(ptrs, winPix, bandCount, nodataInfo, nodata);
            result.resize(winPix);
            ve.applyFormula(*formulaPtr, bf, ptrs, result.data(), winPix, nodata);
        };

        // Determine rescale range. Explicit rescale and formula-defined ranges
        // keep the export fully memory-bounded. The auto-percentile fallback
        // performs an extra windowed pass that collects valid values (only hit
        // when neither an explicit range nor a formula range is available).
        float rMin, rMax;
        if (!rescaleStr.empty()) {
            auto commaPos = rescaleStr.find(',');
            if (commaPos == std::string::npos)
                throw InvalidArgsException("Invalid rescale format");
            rMin = std::stof(rescaleStr.substr(0, commaPos));
            rMax = std::stof(rescaleStr.substr(commaPos + 1));
        } else if (formulaPtr->hasRange && formulaPtr->rangeMin != formulaPtr->rangeMax) {
            rMin = static_cast<float>(formulaPtr->rangeMin);
            rMax = static_cast<float>(formulaPtr->rangeMax);
        } else {
            // Auto-percentile fallback: collect valid values with a windowed
            // pass. To keep memory bounded regardless of raster size, values
            // are uniformly subsampled into a fixed-capacity reservoir instead
            // of storing every pixel.
            constexpr size_t kMaxSamples = 4ull * 1024 * 1024;  // 16 MB of floats
            std::vector<std::vector<float>> storage(bandCount);
            std::vector<float*> ptrs(bandCount);
            std::vector<float> result;
            std::vector<float> valid;
            valid.reserve(std::min<size_t>(kMaxSamples,
                static_cast<size_t>(width) * height));
            // Deterministic, thread-local RNG (fixed seed) for reproducible
            // reservoir sampling, independent of the global std::rand() state.
            std::mt19937 reservoirRng(1337u);
            std::uniform_real_distribution<double> reservoirUni(0.0, 1.0);
            size_t seen = 0;
            for (int ty = 0; ty < nty; ty++) {
                for (int tx = 0; tx < ntx; tx++) {
                    cancel::throwIfCanceled();
                    const int x0 = tx * tw, y0 = ty * th;
                    const int winW = std::min(tw, width - x0);
                    const int winH = std::min(th, height - y0);
                    computeWindow(x0, y0, winW, winH, storage, ptrs, result);
                    for (float v : result) {
                        if (v == nodata || std::isnan(v)) continue;
                        seen++;  // count this element before drawing (Algorithm R)
                        if (valid.size() < kMaxSamples) {
                            valid.push_back(v);
                        } else {
                            // Reservoir sampling: draw j from [0, seen) where seen
                            // already includes this element → probability k/seen.
                            const size_t j = static_cast<size_t>(reservoirUni(reservoirRng) * seen);
                            if (j < kMaxSamples) valid[j] = v;
                        }
                    }
                }
            }
            if (!valid.empty()) {
                std::sort(valid.begin(), valid.end());
                rMin = valid[static_cast<size_t>(valid.size() * 0.02)];
                rMax = valid[std::min(valid.size() - 1, static_cast<size_t>(valid.size() * 0.98))];
            } else {
                rMin = 0; rMax = 1;
            }
        }

        // Create tiled output GeoTIFF with 4 bands (RGBA)
        GDALDriverH gtiffDrv = GDALGetDriverByName("GTiff");
        if (!gtiffDrv)
            throw GDALException("GTiff driver not available");

        CPLStringList createOpts;
        createOpts.AddString("TILED=YES");
        createOpts.AddString(("BLOCKXSIZE=" + std::to_string(tw)).c_str());
        createOpts.AddString(("BLOCKYSIZE=" + std::to_string(th)).c_str());
        createOpts.AddString("COMPRESS=DEFLATE");
        createOpts.AddString("PREDICTOR=2");
        createOpts.AddString("BIGTIFF=IF_SAFER");
        // Compression threads: this one and those the CPU pool allows (DQ-15), held until
        // the output is closed
        const auto cpu = parallel::tryAcquireThreads(parallel::hardwareThreads());
        createOpts.AddString(parallel::gdalNumThreads(cpu).c_str());

        // Deleted unless the export completes; declared first, so the dataset is closed
        // before the file is deleted
        PartialOutput partialOutput(outputPath);
        // Closed on any throw. On success we close it explicitly after the finalization.
        auto outGuard = adoptDataset(
            GDALCreate(gtiffDrv, outputPath, width, height, 4, GDT_Byte, createOpts.List()));
        if (!outGuard)
            throw GDALException("Cannot create output GeoTIFF");
        GDALDatasetH hOut = datasetHandle(outGuard);

        if (hasGeoTransform) GDALSetGeoTransform(hOut, geoTransform);
        if (!projection.empty()) GDALSetProjection(hOut, projection.c_str());
        GDALSetRasterColorInterpretation(GDALGetRasterBand(hOut, 1), GCI_RedBand);
        GDALSetRasterColorInterpretation(GDALGetRasterBand(hOut, 2), GCI_GreenBand);
        GDALSetRasterColorInterpretation(GDALGetRasterBand(hOut, 3), GCI_BlueBand);
        GDALSetRasterColorInterpretation(GDALGetRasterBand(hOut, 4), GCI_AlphaBand);

        std::vector<std::vector<float>> storage(bandCount);
        std::vector<float*> ptrs(bandCount);
        std::vector<float> result;
        std::vector<uint8_t> rgba;
        std::vector<uint8_t> chanData;
        size_t doneTiles = 0;

        checkpoint(0.0, "rendering");
        for (int ty = 0; ty < nty; ty++) {
            for (int tx = 0; tx < ntx; tx++) {
                checkpoint(static_cast<double>(doneTiles) / totalTiles, "rendering");

                const int x0 = tx * tw, y0 = ty * th;
                const int winW = std::min(tw, width - x0);
                const int winH = std::min(th, height - y0);
                const size_t winPix = static_cast<size_t>(winW) * winH;

                computeWindow(x0, y0, winW, winH, storage, ptrs, result);

                rgba.resize(winPix * 4);
                ve.applyColormap(result.data(), rgba.data(), winPix, *cmap, rMin, rMax, nodata);

                chanData.resize(winPix);
                for (int ch = 0; ch < 4; ch++) {
                    for (size_t i = 0; i < winPix; i++) chanData[i] = rgba[i * 4 + ch];
                    GDALRasterBandH hBand = GDALGetRasterBand(hOut, ch + 1);
                    if (GDALRasterIO(hBand, GF_Write, x0, y0, winW, winH,
                                     chanData.data(), winW, winH, GDT_Byte, 0, 0) != CE_None)
                        throw GDALException("Cannot write output tile");
                }
                doneTiles++;
            }
        }

        GDALFlushCache(hOut);
        // Add internal overviews for efficient zoomed/partial reads.
        buildInternalOverviews(hOut, width, height);
        outGuard.reset();  // closes hOut
        srcGuard.reset();  // closes hSrcDs
        partialOutput.commit();
        askCallback(1.0, "rendering");
        return;
    } else {
        // Non-formula mode: band selection + rescale, export as GeoTIFF
        std::vector<int> selectedBands;
        try {
            if (!bandsStr.empty()) {
                std::istringstream ss(bandsStr);
                std::string token;
                while (std::getline(ss, token, ',')) {
                    int b = std::stoi(token);
                    if (b < 1 || b > bandCount)
                        throw InvalidArgsException("Band index out of range: " + token);
                    selectedBands.push_back(b);
                }
            } else if (!presetStr.empty()) {
                auto& spm = ddb::SensorProfileManager::instance();
                auto mapping = spm.getBandMappingForPreset(std::string(inputPath), presetStr);
                selectedBands = {mapping.r, mapping.g, mapping.b};
            } else {
                for (int i = 1; i <= std::min(3, bandCount); i++) selectedBands.push_back(i);
            }
        } catch (const std::exception& e) {
            LOGW << "Error parsing bands/preset: " << e.what();
            throw;
        }

        // Use GDALTranslate for band selection + rescale. Its progress goes to the
        // callback, and it stops when the export is canceled.
        checkpoint(0.0, "converting");
        CPLStringList targs;
        targs.AddString("-ot");
        targs.AddString("Byte");

        for (int b : selectedBands) {
            targs.AddString("-b");
            targs.AddString(std::to_string(b).c_str());
        }

        GDALDataType srcType = GDALGetRasterDataType(GDALGetRasterBand(hSrcDs, 1));
        if (srcType != GDT_Byte) {
            if (!rescaleStr.empty()) {
                auto commaPos = rescaleStr.find(',');
                if (commaPos != std::string::npos) {
                    targs.AddString("-scale");
                    targs.AddString(rescaleStr.substr(0, commaPos).c_str());
                    targs.AddString(rescaleStr.substr(commaPos + 1).c_str());
                    targs.AddString("0");
                    targs.AddString("255");
                }
            } else {
                targs.AddString("-scale");
            }
        }

        targs.AddString("-of");
        targs.AddString("GTiff");
        targs.AddString("-co");
        targs.AddString("TILED=YES");
        targs.AddString("-co");
        targs.AddString("COMPRESS=DEFLATE");

        const GdalOptionsPtr<GDALTranslateOptions> psOptions(
            GDALTranslateOptionsNew(targs.List(), nullptr));
        if (!psOptions)
            throw GDALException("Cannot create GDALTranslate options");
        cancel::GdalProgress translateProgress([&](double fraction) { askCallback(fraction, "converting"); });
        translateProgress.attach(psOptions.get());

        PartialOutput partialOutput(outputPath);
        int bUsageError = FALSE;
        auto out = adoptDataset(GDALTranslate(outputPath, hSrcDs, psOptions.get(), &bUsageError));
        srcGuard.reset();  // closes hSrcDs

        if (!out || bUsageError) {
            out.reset();
            cancel::throwIfCanceled();
            throw GDALException("Cannot create output GeoTIFF");
        }

        GDALFlushCache(datasetHandle(out));
        out.reset();
        // Reopen in update mode to add internal overviews (efficient zoomed
        // reads). Best-effort: a failure here is non-fatal.
        if (const auto ovr = adoptDataset(GDALOpen(outputPath, GA_Update))) {
            buildInternalOverviews(datasetHandle(ovr), width, height);
        }
        partialOutput.commit();
        askCallback(1.0, "converting");
    }
}

DDBErr DDBExportRaster(const char* inputPath, const char* outputPath,
                       const char* preset, const char* bands,
                       const char* formula, const char* bandFilter,
                       const char* colormap, const char* rescale) {
    return guarded([&] {
        // Legacy ABI: forward to the block-windowed implementation with auto tile
        // size and no progress callback (only a cancel token cancels it).
        exportRasterImpl(inputPath, outputPath, preset, bands, formula, bandFilter,
                         colormap, rescale, 0, nullptr, nullptr);
    });
}

DDBErr DDBExportRaster2(const char* inputPath, const char* outputPath,
                        const char* preset, const char* bands,
                        const char* formula, const char* bandFilter,
                        const char* colormap, const char* rescale,
                        int tileSize,
                        DDBProgressCallback progress, void* progressUserData) {
    // DDBExportRaster2 has always returned DDBERR_CANCELED (ABI 1.0)
    return guarded<kFrozenCodes | codeBit(DDBERR_CANCELED)>([&] {
        exportRasterImpl(inputPath, outputPath, preset, bands, formula, bandFilter, colormap, rescale,
                         tileSize, progress, progressUserData);
    });
}


DDBErr DDBGetRasterValueInfo(const char *filePath, char **output) {
    return guardedOutput(output, [&] {
        requireText(filePath, "No file path provided");
        requireOutput(output);

        std::string jsonStr = ddb::getRasterValueInfoJson(std::string(filePath));
        return jsonStr;
    });
}

DDBErr DDBGetRasterPointValue(const char *filePath, int x, int y, char **output) {
    return guardedOutput(output, [&] {
        requireText(filePath, "No file path provided");
        requireOutput(output);

        std::string jsonStr = ddb::getRasterPointJson(std::string(filePath), x, y);
        return jsonStr;
    });
}

DDBErr DDBGetRasterAreaStats(const char *filePath, int x0, int y0, int x1, int y1, char **output) {
    return guardedOutput(output, [&] {
        requireText(filePath, "No file path provided");
        requireOutput(output);

        std::string jsonStr = ddb::getRasterAreaStatsJson(std::string(filePath), x0, y0, x1, y1);
        return jsonStr;
    });
}

DDBErr DDBGetRasterProfile(const char *filePath, const char *geoJsonLineString,
                           int samples, char **output) {
    return guardedOutput(output, [&] {
        requireText(filePath, "No file path provided");
        requireText(geoJsonLineString, "No GeoJSON LineString provided");
        requireOutput(output);

        std::string jsonStr = ddb::getRasterProfileJson(std::string(filePath),
                                                        std::string(geoJsonLineString),
                                                        samples);
        return jsonStr;
    });
}

DDBErr DDBCalculateVolume(const char *rasterPath,
                          const char *polygonGeoJSON,
                          const char *baseMethod,
                          double flatElevation,
                          char **output) {
    return guardedOutput(output, [&] {
        requireText(rasterPath, "No raster path provided");
        requireText(polygonGeoJSON, "No polygon GeoJSON provided");
        requireOutput(output);

        const std::string method = optStr(baseMethod);
        std::string jsonStr = ddb::calculateVolumeJson(std::string(rasterPath),
                                                       std::string(polygonGeoJSON),
                                                       method,
                                                       flatElevation);
        return jsonStr;
    });
}

DDBErr DDBDetectStockpile(const char *rasterPath,
                          double lat, double lon,
                          double radius,
                          float sensitivity,
                          char **output) {
    return guardedOutput(output, [&] {
        requireText(rasterPath, "No raster path provided");
        requireOutput(output);

        std::string jsonStr = ddb::detectStockpileJson(std::string(rasterPath),
                                                       lat, lon, radius, sensitivity);
        return jsonStr;
    });
}

DDBErr DDBDetectAllStockpiles(const char *rasterPath,
                              float sensitivity,
                              double minAreaM2,
                              int maxResults,
                              char **output) {
    return guardedOutput(output, [&] {
        requireText(rasterPath, "No raster path provided");
        requireOutput(output);

        std::string jsonStr = ddb::detectAllStockpilesJson(std::string(rasterPath),
                                                           sensitivity,
                                                           minAreaM2,
                                                           maxResults);
        return jsonStr;
    });
}

DDBErr DDBGenerateContours(const char *rasterPath,
                           double interval,
                           int count,
                           double baseOffset,
                           double minElev,
                           double maxElev,
                           double simplifyTolerance,
                           int bandIndex,
                           char **output) {
    return guardedOutput(output, [&] {
        requireText(rasterPath, "No raster path provided");
        requireOutput(output);

        ContourOptions opts;
        if (interval > 0.0) opts.interval = interval;
        if (count > 0) opts.count = count;
        opts.baseOffset = baseOffset;
        if (std::isfinite(minElev)) opts.minElev = minElev;
        if (std::isfinite(maxElev)) opts.maxElev = maxElev;
        opts.simplifyTolerance = std::max(0.0, simplifyTolerance);
        opts.bandIndex = (bandIndex > 0) ? bandIndex : 1;

        std::string jsonStr = ddb::generateContoursJson(std::string(rasterPath), opts);
        return jsonStr;
    });
}

DDBErr DDBMaskBorders(const char *input, const char *output, int nearDist, bool white) {
    return guarded([&] {
        requireText(input, "No input path provided");
        requireText(output, "No output path provided");

        ddb::maskBorders(std::string(input), std::string(output), nearDist, white);
    });
}


// =====================================================================
// OGC services C API (raster region rendering + vector query/describe).
// =====================================================================

DDBErr DDBRenderRasterRegion(const char *inputPath,
                             const double *bbox,
                             const char *bboxSrs,
                             int width, int height,
                             const char *format,
                             uint8_t **outBuffer,
                             int *outBufferSize) {
    // Legacy entry point: delegate to the extended variant with no band
    // selection and no explicit output CRS (target SRS = bbox SRS).
    return DDBRenderRasterRegionEx(inputPath, bbox, bboxSrs, nullptr,
                                   nullptr, 0,
                                   width, height, format,
                                   outBuffer, outBufferSize);
}

DDBErr DDBRenderRasterRegionEx(const char *inputPath,
                               const double *bbox,
                               const char *bboxSrs,
                               const char *outputCrs,
                               const int *bands,
                               int bandCount,
                               int width, int height,
                               const char *format,
                               uint8_t **outBuffer,
                               int *outBufferSize) {
    return guardedBuffer(outBuffer, outBufferSize, [&](uint8_t** buffer, int* bufferSize) {
        requireText(inputPath, "No input path provided");
        requireOutput(bbox, "No bbox provided");
        requireOutput(outBuffer, "outBuffer is null");
        requireOutput(outBufferSize, "outBufferSize is null");
        if (bandCount < 0)
            throw InvalidArgsException("bandCount is negative");
        if (bandCount > 0 && bands == nullptr)
            throw InvalidArgsException("bands is null but bandCount > 0");

        std::vector<int> bandVec;
        if (bandCount > 0) bandVec.assign(bands, bands + bandCount);

        ddb::renderRasterRegion(std::string(inputPath), bbox, optStr(bboxSrs),
                                width, height, optStr(format), buffer, bufferSize,
                                optStr(outputCrs), bandVec);
    });
}

DDBErr DDBRenderRasterIndex(const char *inputPath,
                            const char *indexName,
                            const double *bbox,
                            const char *bboxSrs,
                            int width, int height,
                            const char *format,
                            uint8_t **outBuffer,
                            int *outBufferSize) {
    return guardedBuffer(outBuffer, outBufferSize, [&](uint8_t** buffer, int* bufferSize) {
        requireText(inputPath, "No input path provided");
        requireText(indexName, "No indexName provided");
        requireOutput(bbox, "No bbox provided");
        if (outBuffer == nullptr || outBufferSize == nullptr)
            throw InvalidArgsException("outBuffer/outBufferSize is null");

        ddb::renderRasterIndex(std::string(inputPath), std::string(indexName),
                               bbox, optStr(bboxSrs), width, height, optStr(format),
                               buffer, bufferSize);
    });
}

DDBErr DDBQueryRasterPoint(const char *inputPath,
                           double x, double y,
                           const char *srs,
                           char **output) {
    return guardedOutput(output, [&] {
        requireText(inputPath, "No input path provided");
        requireOutput(output, "output is null");

        const std::string s = optStr(srs);
        const std::string j = ddb::queryRasterPoint(std::string(inputPath), x, y, s);
        return j;
    });
}

namespace {
    std::string mapVectorMime(const char *mime) {
        if (!mime || !*mime) return "geojson";
        std::string m(mime);
        if (m == "application/json" || m == "application/geo+json" ||
            m == "geojson" || m == "json")
            return "geojson";
        if (m == "application/gml+xml" || m == "text/xml" || m == "gml")
            return "gml";
        // Anything else: let queryVector throw a clear error.
        return m;
    }
}

namespace {
// The selection part of a vector query, validated
VectorQuery vectorSelection(const char *vectorPath, const char *layerName, const double *bbox,
                            const char *bboxSrs, const long long *fids, int numFids, const char *where) {
    requireText(vectorPath, "No vector path provided");
    if (numFids < 0 || (numFids > 0 && fids == nullptr))
        throw InvalidArgsException("Invalid feature id list");

    VectorQuery query;
    query.layerName = optStr(layerName);
    if (bbox != nullptr)
        query.bbox = std::array<double, 4>{bbox[0], bbox[1], bbox[2], bbox[3]};
    query.bboxSrs = optStr(bboxSrs);
    query.fids.assign(fids, fids + numFids);
    query.where = optStr(where);
    return query;
}
}  // namespace

DDBErr DDBQueryVector(const char *vectorPath,
                      const char *layerName,
                      const double *bbox,
                      const char *bboxSrs,
                      int maxFeatures,
                      int startIndex,
                      const char *outputFormat,
                      char **output) {
    return DDBQueryVectorEx(vectorPath, layerName, bbox, bboxSrs, nullptr, 0, nullptr, maxFeatures, startIndex,
                            outputFormat, output);
}

DDBErr DDBQueryVectorEx(const char *vectorPath,
                        const char *layerName,
                        const double *bbox,
                        const char *bboxSrs,
                        const long long *fids,
                        int numFids,
                        const char *where,
                        int maxFeatures,
                        int startIndex,
                        const char *outputFormat,
                        char **output) {
    return guardedOutput(output, [&] {
        requireOutput(output, "output is null");

        auto query = vectorSelection(vectorPath, layerName, bbox, bboxSrs, fids, numFids, where);
        query.outputFormat = mapVectorMime(outputFormat);

        // Clamp maxFeatures to spec range [1, 10000]; 0 -> default 1000.
        int mf = maxFeatures;
        if (mf <= 0) mf = 1000;
        if (mf > 10000) mf = 10000;
        query.maxFeatures = mf;
        query.startIndex = startIndex < 0 ? 0 : startIndex;

        return ddb::queryVector(std::string(vectorPath), query);
    });
}

DDBErr DDBCountVector(const char *vectorPath,
                      const char *layerName,
                      const double *bbox,
                      const char *bboxSrs,
                      const long long *fids,
                      int numFids,
                      const char *where,
                      long long *count) {
    return guardedOutput(count, [&] {
        requireOutput(count, "count is null");

        const auto query = vectorSelection(vectorPath, layerName, bbox, bboxSrs, fids, numFids, where);
        return ddb::countVectorFeatures(std::string(vectorPath), query);
    });
}

DDBErr DDBDescribeVector(const char *vectorPath,
                         const char *layerName,
                         char **output) {
    return guardedOutput(output, [&] {
        requireText(vectorPath, "No vector path provided");
        requireOutput(output, "output is null");

        const std::string layer = optStr(layerName);
        const std::string j = ddb::describeVector(std::string(vectorPath), layer);
        return j;
    });
}

DDBErr DDBMakeCadPrj(const char *crs, char **output) {
    return guardedOutput(output, [&] {
        requireOutput(crs, "No coordinate system provided");
        requireOutput(output, "output is null");

        return ddb::cad::prjContent(crs);
    });
}
