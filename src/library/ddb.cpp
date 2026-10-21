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
#include <span>
#include <string_view>
#include <utility>

#include <sstream>

#ifdef WIN32
#include <windows.h>
#endif

#include "build.h"
#include "build_budget.h"
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
#include "gdal_inc.h"
#include "hash.h"
#include "info.h"
#include "json.h"
#include "logger.h"
#include "mask.h"
#include "align.h"
#include "merge_multispectral.h"
#include "mio.h"
#include "passwordmanager.h"
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

// thread_local: each thread's failing call gets its own buffer, so a cross-thread failure never
// clobbers another thread's error message (was a plain global; a real data race under concurrency).
thread_local char ddbLastError[255];

void DDBRegisterProcess(bool verbose) {
    ddb::registerProcess(verbose);
}

const char* DDBGetVersion() {
    return ddb::version();
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
                                    DDB_ERROR_CODE(NOTFOUND)};
#undef DDB_ERROR_CODE

// The exports that return DDBERR_CANCELED when their DDBProgressCallback asks to stop
constexpr std::string_view kCancellable[] = {"DDBExportRaster2"};

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
    return json{{"abi", {{"major", DDB_ABI_MAJOR}, {"minor", DDB_ABI_MINOR}}},
                {"version", ddb::version()},
                {"errorCodes", std::move(errorCodes)},
                {"cancellable", stringArray(kCancellable)},
                {"symbols", stringArray(kExportedSymbols)}}
        .dump();
}

}  // namespace

// Exempt accessors (no DDB_C_BEGIN): they never read or change the last error
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

DDBErr DDBNormalizePath(const char* path, const char* reservedJson, char** output) {
    DDB_C_BEGIN
    if (path == nullptr)
        throw InvalidArgsException("No path provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    const auto rules = ReservedRules::fromJson(reservedJson);
    utils::copyToPtr(normalizeEntryPath(path, rules).toJson().dump(), output);
    DDB_C_END
}

DDBErr DDBInit(const char* directory, char** outPath) {
    DDB_C_BEGIN
    if (directory == nullptr)
        throw InvalidArgsException("No directory provided");

    if (outPath == nullptr)
        throw InvalidArgsException("No output provided");

    const std::string ddbDirPath = initIndex(directory);
    utils::copyToPtr(ddbDirPath, outPath);

    DDB_C_END
}

const char* DDBGetLastError() {
    return ddbLastError;
}

void DDBSetLastError(const char* err) {
    strncpy(ddbLastError, err, 255);
    ddbLastError[254] = '\0';
}

DDBErr DDBAdd(const char* ddbPath,
              const char** paths,
              int numPaths,
              char** output,
              bool recursive) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidArrayParam(paths, numPaths))
        throw InvalidArgsException("Invalid paths array parameter");

    if (utils::isNullOrEmptyOrWhitespace(paths, numPaths))
        throw InvalidArgsException("No paths provided");

    if (utils::hasNullStringInArray(paths, numPaths))
        throw InvalidArgsException("Path array contains null elements");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    const auto db = ddb::open(std::string(ddbPath), true);
    const std::vector<std::string> pathList(paths, paths + numPaths);
    auto outJson = json::array();
    addToIndex(db.get(),
               ddb::expandPathList(pathList, recursive, 0),
               [&outJson](const Entry& e, bool) {
                   json j;
                   e.toJSON(j);
                   outJson.push_back(j);
                   return true;
               });

    utils::copyToPtr(outJson.dump(), output);
    DDB_C_END
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
    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (options == nullptr)
        throw InvalidArgsException("Options pointer is null");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");
}

// { "entries": [...], "unchanged": [...], "errors": [...] }
static std::string addResultJson(const AddResult &result) {
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

    return outJson.dump();
}

DDBErr DDBAddWithOptions(const char* ddbPath,
                         const char** paths,
                         int numPaths,
                         const DDBAddOptions* options,
                         char** output) {
    DDB_C_BEGIN

    if (!utils::isValidArrayParam(paths, numPaths))
        throw InvalidArgsException("Invalid paths array parameter");

    if (utils::isNullOrEmptyOrWhitespace(paths, numPaths))
        throw InvalidArgsException("No paths provided");

    if (utils::hasNullStringInArray(paths, numPaths))
        throw InvalidArgsException("Path array contains null elements");

    validateAddArgs(ddbPath, options, output);

    const auto db = ddb::open(std::string(ddbPath), true);
    const std::vector<std::string> pathList(paths, paths + numPaths);
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

    utils::copyToPtr(addResultJson(result), output);
    DDB_C_END
}

DDBErr DDBAddWithHints(const char *ddbPath, const DDBAddHint *files, int numFiles,
                       const DDBAddOptions *options, char **output) {
    DDB_C_BEGIN

    if (files == nullptr || numFiles <= 0)
        throw InvalidArgsException("No files provided");

    validateAddArgs(ddbPath, options, output);

    const auto db = ddb::open(std::string(ddbPath), true);
    const auto addOpts = toAddOptions(options);

    AddResult result;
    std::vector<KnownFile> known;
    known.reserve(numFiles);
    for (int i = 0; i < numFiles; i++) {
        const auto &f = files[i];
        if (utils::isNullOrEmptyOrWhitespace(f.path))
            throw InvalidArgsException("File " + std::to_string(i) + " has no path");

        const std::string p(f.path);
        if (!fs::exists(p)) {
            // A missing file would abort the whole batch in addToIndexEx
            const FSException ex(p + " does not exist");
            if (addOpts.stopOnError) throw ex;
            addPathError(db.get(), p, ex, result);
            continue;
        }

        known.push_back({p, f.sha256 == nullptr ? std::string() : std::string(f.sha256), f.size, f.mtime});
    }

    addToIndexEx(db.get(), known, addOpts, result);

    utils::copyToPtr(addResultJson(result), output);
    DDB_C_END
}

DDBErr DDBRemove(const char* ddbPath, const char** paths, int numPaths) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidArrayParam(paths, numPaths))
        throw InvalidArgsException("Invalid paths array parameter");

    if (utils::isNullOrEmptyOrWhitespace(paths, numPaths))
        throw InvalidArgsException("No paths provided");

    if (utils::hasNullStringInArray(paths, numPaths))
        throw InvalidArgsException("Path array contains null elements");

    const auto db = ddb::open(std::string(ddbPath), true);
    const std::vector<std::string> pathList(paths, paths + numPaths);

    removeFromIndex(db.get(), pathList);
    DDB_C_END
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
    DDB_C_BEGIN

    if (!utils::isValidNonEmptyStringParam(format))
        throw InvalidArgsException("No format provided");

    if (!utils::isValidNonEmptyStringParam(geometry))
        throw InvalidArgsException("No geometry provided");

    if (!utils::isValidArrayParam(paths, numPaths))
        throw InvalidArgsException("Invalid paths array parameter");

    if (utils::isNullOrEmptyOrWhitespace(paths, numPaths))
        throw InvalidArgsException("No paths provided");

    if (utils::hasNullStringInArray(paths, numPaths))
        throw InvalidArgsException("Path array contains null elements");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    if (maxRecursionDepth < 0)
        throw InvalidArgsException("Invalid max recursion depth");

    const std::vector<std::string> input(paths, paths + numPaths);
    std::ostringstream ss;
    info(input, ss, format, recursive, maxRecursionDepth, geometry, withHash, stopOnError);
    utils::copyToPtr(ss.str(), output);
    DDB_C_END
}

DDBErr DDBReleaseConnections(const char* path, int* closed) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(path))
        throw InvalidArgsException("No path provided");

    const auto count = ddb::DatabasePool::instance().releaseConnections(fs::path(path)) +
                       ddb::releaseCachedFiles(fs::path(path));
    if (closed != nullptr)
        *closed = static_cast<int>(count);

    DDB_C_END
}

DDBErr DDBCheckpoint(const char* ddbPath, int* complete) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    const bool done = ddb::checkpointIndex(std::string(ddbPath));
    if (complete != nullptr)
        *complete = done ? 1 : 0;

    DDB_C_END
}

DDBErr DDBGet(const char* ddbPath, const char* path, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (utils::isNullOrEmptyOrWhitespace(path))
        throw InvalidArgsException("No path provided");

    const auto db = ddb::open(std::string(ddbPath), false);

    ddb::Entry entry;
    if (!ddb::getEntryWithMeta(db.get(), std::string(path), entry)) {
        DDBSetLastError(("No entry " + std::string(path)).c_str());
        return DDBERR_NOTFOUND;
    }
    utils::copyToPtr(entry.toJSONString(), output);

    DDB_C_END
}

DDBErr DDBList(const char* ddbPath,
               const char** paths,
               int numPaths,
               char** output,
               const char* format,
               bool recursive,
               int maxRecursionDepth) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidNonEmptyStringParam(format))
        throw InvalidArgsException("No format provided");

    if (!utils::isValidArrayParam(paths, numPaths))
        throw InvalidArgsException("Invalid paths array parameter");

    if (utils::isNullOrEmptyOrWhitespace(paths, numPaths))
        throw InvalidArgsException("No paths provided");

    if (utils::hasNullStringInArray(paths, numPaths))
        throw InvalidArgsException("Path array contains null elements");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    if (maxRecursionDepth < 0)
        throw InvalidArgsException("Invalid max recursion depth");

    const auto db = ddb::open(std::string(ddbPath), true);
    const std::vector<std::string> pathList(paths, paths + numPaths);

    std::ostringstream ss;
    listIndex(db.get(), pathList, ss, format, recursive, maxRecursionDepth);

    utils::copyToPtr(ss.str(), output);

    DDB_C_END
}

DDBErr DDBListEntries(const char* ddbPath, const char* folder, bool recursive, const char* namePattern,
                      const char* types, bool withDetails, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    EntryQuery query;
    query.folder = folder != nullptr ? folder : "";
    query.recursive = recursive;
    query.namePattern = namePattern != nullptr ? namePattern : "";
    query.types = utils::parseEntryTypeList(types, /*allowDirectory=*/true);
    query.withDetails = withDetails;

    const auto db = ddb::open(std::string(ddbPath), false);

    std::ostringstream ss;
    writeEntries(ss, listEntries(db.get(), query), "json");
    utils::copyToPtr(ss.str(), output);

    DDB_C_END
}

DDBErr DDBSearch(const char* ddbPath, const char* query, char** output, const char* format) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidStringParam(query))
        throw InvalidArgsException("No query provided");

    if (!utils::isValidNonEmptyStringParam(format))
        throw InvalidArgsException("No format provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    const auto db = ddb::open(std::string(ddbPath), false);

    std::ostringstream ss;
    searchIndex(db.get(), query, ss, format);

    utils::copyToPtr(ss.str(), output);

    DDB_C_END
}

DDBErr DDBAppendPassword(const char* ddbPath, const char* password) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidNonEmptyStringParam(password))
        throw InvalidArgsException("No password provided");

    const auto db = ddb::open(std::string(ddbPath), true);

    PasswordManager manager(db.get());

    manager.append(std::string(password));

    DDB_C_END
}

DDBErr DDBVerifyPassword(const char* ddbPath, const char* password, bool* verified) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    // We allow empty password verification
    if (password == nullptr)
        throw InvalidArgsException("No password provided");

    if (verified == nullptr)
        throw InvalidArgsException("Output parameter pointer is null");

    const auto db = ddb::open(std::string(ddbPath), true);

    PasswordManager manager(db.get());

    *verified = manager.verify(std::string(password));

    DDB_C_END
}

DDBErr DDBClearPasswords(const char* ddbPath) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    const auto db = ddb::open(std::string(ddbPath), true);

    PasswordManager manager(db.get());

    manager.clearAll();

    DDB_C_END
}

DDB_DLL DDBErr DDBStatus(const char* ddbPath, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (output == nullptr)
        throw InvalidArgsException("No output provided");

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

    utils::copyToPtr(ss.str(), output);

    DDB_C_END
}

// @deprecated
// @deprecated
DDBErr DDBChattr(const char* ddbPath, const char* attrsJson, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidStringParam(attrsJson))
        throw InvalidArgsException("No attributes JSON provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    LOGD << "Deprecated DDBChattr call: please use DDBMetaSet instead as DDBChattr will be removed "
            "in the near future.";

    const auto db = ddb::open(std::string(ddbPath), true);
    try {
        const json j = json::parse(attrsJson);
        for (auto& el : j.items()) {
            if (el.key() == "public" && el.value().is_boolean()) {
                db->getMetaManager()->set("visibility", el.value() ? "1" : "0");
            } else {
                throw InvalidArgsException("Invalid attribute " + el.key());
            }
        }
        utils::copyToPtr(db->getProperties().dump(), output);
    } catch (const json::parse_error& e) {
        throw InvalidArgsException(e.what());
    }
    DDB_C_END
}

DDBErr DDBGenerateThumbnail(const char* filePath, int size, const char* destPath) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(filePath))
        throw InvalidArgsException("No file path provided");

    if (utils::isNullOrEmptyOrWhitespace(destPath))
        throw InvalidArgsException("No destination path provided");

    if (size < 0)
        throw InvalidArgsException("Invalid size parameter");

    const auto imagePath = fs::path(filePath);
    const auto thumbPath = fs::path(destPath);

    generateThumb(imagePath, size, thumbPath, true, nullptr, nullptr);

    DDB_C_END
}

DDB_DLL DDBErr DDBGenerateMemoryThumbnail(const char* filePath,
                                          int size,
                                          uint8_t** outBuffer,
                                          int* outBufferSize) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(filePath))
        throw InvalidArgsException("No file path provided");

    if (size < 0)
        throw InvalidArgsException("Invalid size parameter");

    if (outBuffer == nullptr)
        throw InvalidArgsException("Output buffer pointer is null");

    if (outBufferSize == nullptr)
        throw InvalidArgsException("Output buffer size pointer is null");

    const auto imagePath = fs::path(filePath);

    generateThumb(imagePath, size, "", true, outBuffer, outBufferSize);

    DDB_C_END
}

DDBErr DDBVSIFree(uint8_t* buffer) {
    DDB_C_BEGIN
    // nullptr is allowed - matches C free() semantics (no-op)
    if (buffer != nullptr)
        VSIFree(buffer);
    DDB_C_END
}

DDBErr DDBFree(char* ptr) {
    DDB_C_BEGIN
    // ptr == nullptr is allowed (no-op, matches standard free behaviour)
    free(ptr);
    DDB_C_END
}

DDB_DLL DDBErr DDBTile(const char* inputPath,
                       int tz,
                       int tx,
                       int ty,
                       char** outputTilePath,
                       int tileSize,
                       bool tms,
                       bool forceRecreate) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(inputPath))
        throw InvalidArgsException("No input path provided");

    if (outputTilePath == nullptr)
        throw InvalidArgsException("Output tile path pointer is null");

    if (tileSize < 0)
        throw InvalidArgsException("Invalid tile size parameter");

    if (tz < 0 || tx < 0 || ty < 0)
        throw InvalidArgsException("Invalid tile coordinates");

    const auto tilePath = ddb::TilerHelper::getFromUserCache(std::string(inputPath),
                                                             tz,
                                                             tx,
                                                             ty,
                                                             tileSize,
                                                             tms,
                                                             forceRecreate);
    utils::copyToPtr(tilePath.string(), outputTilePath);
    DDB_C_END
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
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(inputPath))
        throw InvalidArgsException("No input path provided");

    if (outBuffer == nullptr)
        throw InvalidArgsException("Output buffer pointer is null");

    if (outBufferSize == nullptr)
        throw InvalidArgsException("Output buffer size pointer is null");

    if (tileSize < 0)
        throw InvalidArgsException("Invalid tile size parameter");

    if (tz < 0 || tx < 0 || ty < 0)
        throw InvalidArgsException("Invalid tile coordinates");

    // inputPathHash can be null or empty, it's optional
    const std::string hashStr = inputPathHash ? std::string(inputPathHash) : "";

    ddb::TilerHelper::getTile(std::string(inputPath),
                              tz,
                              tx,
                              ty,
                              tileSize,
                              tms,
                              forceRecreate,
                              "",
                              outBuffer,
                              outBufferSize,
                              hashStr);
    DDB_C_END
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
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(inputPath))
        throw InvalidArgsException("No input path provided");
    if (outBuffer == nullptr)
        throw InvalidArgsException("Output buffer pointer is null");
    if (outBufferSize == nullptr)
        throw InvalidArgsException("Output buffer size pointer is null");
    if (tileSize < 0)
        throw InvalidArgsException("Invalid tile size parameter");
    if (tz < 0 || tx < 0 || ty < 0)
        throw InvalidArgsException("Invalid tile coordinates");

    const std::string hashStr = inputPathHash ? std::string(inputPathHash) : "";
    const std::string fmtStr = (outputFormat && *outputFormat) ? std::string(outputFormat) : std::string("png");

    ddb::TilerHelper::getTile(std::string(inputPath),
                              tz,
                              tx,
                              ty,
                              tileSize,
                              tms,
                              forceRecreate,
                              "",
                              fmtStr,
                              outBuffer,
                              outBufferSize,
                              hashStr);
    DDB_C_END
}

DDBErr DDBDelta(const char* ddbSourceStamp,
                const char* ddbTargetStamp,
                char** output,
                const char* format) {
    DDB_C_BEGIN

    if (ddbSourceStamp == nullptr)
        throw InvalidArgsException("No ddb source path provided");

    if (ddbTargetStamp == nullptr)
        throw InvalidArgsException("No ddb path provided");

    if (format == nullptr || strlen(format) == 0)
        throw InvalidArgsException("No format provided");

    if (output == nullptr)
        throw InvalidArgsException("No output provided");

    std::ostringstream ss;

    try {
        const json source = json::parse(ddbSourceStamp);
        const json dest = json::parse(ddbTargetStamp);

        delta(source, dest, ss, format);

        utils::copyToPtr(ss.str(), output);
    } catch (const json::parse_error& e) {
        throw InvalidArgsException(e.what());
    }

    DDB_C_END
}

DDB_DLL DDBErr DDBApplyDelta(const char* delta,
                             const char* sourcePath,
                             const char* ddbPath,
                             int mergeStrategy,
                             const char* sourceMetaDump,
                             char** conflicts) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(delta))
        throw InvalidArgsException("No delta provided");

    if (utils::isNullOrEmptyOrWhitespace(sourcePath))
        throw InvalidArgsException("No source path provided");

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No ddb path provided");

    if (utils::isNullOrEmptyOrWhitespace(sourceMetaDump))
        throw InvalidArgsException("No source meta dump provided");

    if (conflicts == nullptr)
        throw InvalidArgsException("Conflicts output pointer is null");

    if (mergeStrategy < 0)
        throw InvalidArgsException("Invalid merge strategy");

    Delta d;
    json metaDump;
    try {
        d = json::parse(delta);
        metaDump = json::parse(sourceMetaDump);
    } catch (const json::parse_error& e) {
        throw InvalidArgsException(e.what());
    }

    // The added files must match the hashes of the delta (DDB_VERIFY_PUSH_HASHES=0 skips the
    // check, for hosts that verify the uploaded bytes themselves)
    const bool verifyHashes = [] {
        const char* env = std::getenv("DDB_VERIFY_PUSH_HASHES");
        return env == nullptr || (env[0] != '0' && env[0] != '\0');
    }();

    const auto ddb = ddb::open(std::string(ddbPath), false);
    std::stringstream ss;
    const auto adc = applyDelta(d,
                                fs::path(std::string(sourcePath)),
                                ddb.get(),
                                static_cast<MergeStrategy>(mergeStrategy),
                                metaDump,
                                ss,
                                verifyHashes);

    json j = json::array();
    for (auto& c : adc)
        j.push_back(c.path);
    utils::copyToPtr(j.dump(), conflicts);

    DDB_C_END
}

DDBErr DDBComputeDeltaLocals(const char* delta,
                             const char* ddbPath,
                             const char* hlDestFolder,
                             char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(delta))
        throw InvalidArgsException("No delta provided");

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    // hlDestFolder can be empty string, but not null
    if (hlDestFolder == nullptr)
        throw InvalidArgsException("Destination folder parameter is null");

    Delta d;
    try {
        d = json::parse(delta);
    } catch (const json::parse_error& e) {
        throw InvalidArgsException(e.what());
    }
    const auto ddb = ddb::open(std::string(ddbPath), false);

    auto cdl = computeDeltaLocals(d, ddb.get(), std::string(hlDestFolder));
    json j = json::object();
    for (auto& el : cdl)
        j[el.first] = el.second;
    utils::copyToPtr(j.dump(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBSetTag(const char* ddbPath, const char* newTag) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No ddb path provided");

    if (!utils::isValidStringParam(newTag))
        throw InvalidArgsException("No tag provided");

    const auto ddb = ddb::open(std::string(ddbPath), true);
    TagManager manager(ddb.get());
    manager.setTag(newTag);

    DDB_C_END
}

DDB_DLL DDBErr DDBGetTag(const char* ddbPath, char** outTag) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (outTag == nullptr)
        throw InvalidArgsException("Output tag pointer is null");

    const auto ddb = ddb::open(std::string(ddbPath), true);
    TagManager manager(ddb.get());
    const auto tag = manager.getTag();

    utils::copyToPtr(tag, outTag);

    DDB_C_END
}

DDB_DLL DDBErr DDBGetStamp(const char* ddbPath, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (output == nullptr)
        throw InvalidArgsException("No output provided");

    const auto ddb = ddb::open(std::string(ddbPath), true);
    utils::copyToPtr(ddb->getStampJson(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBGetStampChecksum(const char* ddbPath, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (output == nullptr)
        throw InvalidArgsException("No output provided");

    const auto ddb = ddb::open(std::string(ddbPath), true);
    utils::copyToPtr(ddb->getStampChecksum(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBRescan(const char* ddbPath, char** output, const char* types, bool stopOnError) {
    return DDBRescanEx(ddbPath, output, types, "", stopOnError);
}

DDB_DLL DDBErr DDBRescanEx(const char* ddbPath, char** output, const char* types, const char* extensions,
                           bool stopOnError) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

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

    utils::copyToPtr(outJson.dump(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBMoveEntry(const char* ddbPath, const char* source, const char* dest) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (utils::isNullOrEmptyOrWhitespace(source))
        throw InvalidArgsException("No source path provided");
    if (utils::isNullOrEmptyOrWhitespace(dest))
        throw InvalidArgsException("No dest path provided");

    const auto ddb = ddb::open(std::string(ddbPath), true);

    moveEntry(ddb.get(), std::string(source), std::string(dest));

    DDB_C_END
}

DDB_DLL DDBErr
DDBBuild(const char* ddbPath, const char* source, const char* dest, bool force, bool pendingOnly) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No ddb path provided");

    const auto ddb = ddb::open(std::string(ddbPath), true);

    // dest can be null (optional parameter)
    const auto destPath = dest != nullptr ? std::string(dest) : "";

    // source can be null (optional parameter)
    std::string path;
    if (source != nullptr)
        path = std::string(source);

    try {
        if (path.empty()) {
            if (pendingOnly)
                buildPending(ddb.get(), destPath, force);
            else
                buildAll(ddb.get(), destPath, force);
        } else {
            build(ddb.get(), path, destPath, force);
        }
    } catch (const ddb::BuildDepMissingException& e) {
        DDBSetLastError(e.what());
        return DDBERR_BUILDDEPMISSING;
    } catch (const ddb::BuildInProgressException& e) {
        DDBSetLastError(e.what());
        return DDBERR_BUILDINPROGRESS;
    }

    DDB_C_END
}

DDB_DLL DDBErr DDBCleanup(const char* ddbPath, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No ddb path provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    const auto ddb = ddb::open(std::string(ddbPath), true);

    const auto result = cleanupBuild(ddb.get(), "");

    json j;
    j["entries"] = result.removedEntries;
    j["builds"] = result.removedBuilds;

    utils::copyToPtr(j.dump(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBIsBuildable(const char* ddbPath, const char* path, bool* isBuildable) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidStringParam(path))
        throw InvalidArgsException("No path provided");

    if (isBuildable == nullptr)
        throw InvalidArgsException("Buildable parameter is null");

    const auto ddbPathStr = std::string(ddbPath);

    const auto ddb = ddb::open(ddbPathStr, true);

    std::string subfolder;

    *isBuildable = ddb::isBuildable(ddb.get(), std::string(path), subfolder);

    DDB_C_END
}

DDB_DLL DDBErr DDBIsBuildPending(const char* ddbPath, bool* isBuildPending) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (isBuildPending == nullptr)
        throw InvalidArgsException("isBuildPending parameter is null");

    const auto ddb = ddb::open(std::string(ddbPath), true);
    *isBuildPending = ddb::isBuildPending(ddb.get());

    DDB_C_END
}

DDB_DLL DDBErr DDBGetPendingBuildInfo(const char* ddbPath, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

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

    utils::copyToPtr(outJson.dump(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBIsBuildActive(const char* ddbPath, const char* path, bool* isBuildActive) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (utils::isNullOrEmptyOrWhitespace(path))
        throw InvalidArgsException("No path provided");

    if (isBuildActive == nullptr)
        throw InvalidArgsException("isBuildActive parameter is null");

    const auto ddb = ddb::open(std::string(ddbPath), true);
    *isBuildActive = ddb::isBuildActive(ddb.get(), std::string(path));

    DDB_C_END
}

DDB_DLL DDBErr DDBIsBuildComplete(const char* ddbPath, const char* path, bool* isBuildComplete) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (utils::isNullOrEmptyOrWhitespace(path))
        throw InvalidArgsException("No path provided");

    if (isBuildComplete == nullptr)
        throw InvalidArgsException("isBuildComplete parameter is null");

    const auto ddb = ddb::open(std::string(ddbPath), true);
    *isBuildComplete = ddb::isBuildComplete(ddb.get(), std::string(path));

    DDB_C_END
}

DDB_DLL DDBErr DDBGetBuildStatus(const char* ddbPath, const char** paths, int numPaths, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidArrayParam(paths, numPaths))
        throw InvalidArgsException("Invalid paths array parameter");

    if (utils::hasNullStringInArray(paths, numPaths))
        throw InvalidArgsException("Path array contains null elements");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    const auto ddb = ddb::open(std::string(ddbPath), true);
    const auto statuses = ddb::getBuildStatus(ddb.get(), std::vector<std::string>(paths, paths + numPaths));

    json j = json::array();
    for (const auto& s : statuses)
        j.push_back({{"path", s.path}, {"exists", s.exists}, {"buildable", s.buildable}, {"complete", s.complete}});

    utils::copyToPtr(j.dump(), output);

    DDB_C_END
}

DDBErr DDBMetaAdd(const char* ddbPath,
                  const char* path,
                  const char* key,
                  const char* data,
                  char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidStringParam(path))
        throw InvalidArgsException("No path provided");

    if (!utils::isValidStringParam(key))
        throw InvalidArgsException("No key provided");

    if (!utils::isValidStringParam(data))
        throw InvalidArgsException("No data provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    const auto ddbPathStr = std::string(ddbPath);

    const auto ddb = ddb::open(ddbPathStr, true);
    auto json = ddb->getMetaManager()->add(std::string(key),
                                           std::string(data),
                                           std::string(path),
                                           ddbPathStr);

    utils::copyToPtr(json.dump(), output);

    DDB_C_END
}

DDBErr DDBMetaSet(const char* ddbPath,
                  const char* path,
                  const char* key,
                  const char* data,
                  char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidStringParam(path))
        throw InvalidArgsException("No path provided");

    if (!utils::isValidStringParam(key))
        throw InvalidArgsException("No key provided");

    if (!utils::isValidStringParam(data))
        throw InvalidArgsException("No data provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    const auto ddbPathStr = std::string(ddbPath);

    const auto ddb = ddb::open(ddbPathStr, true);
    auto json = ddb->getMetaManager()->set(std::string(key),
                                           std::string(data),
                                           std::string(path),
                                           ddbPathStr);

    utils::copyToPtr(json.dump(), output);

    DDB_C_END
}

DDBErr DDBMetaRemove(const char* ddbPath, const char* id, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidStringParam(id))
        throw InvalidArgsException("No id provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    const auto ddb = ddb::open(std::string(ddbPath), true);
    auto json = ddb->getMetaManager()->remove(std::string(id));

    utils::copyToPtr(json.dump(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBMetaGet(const char* ddbPath, const char* path, const char* key, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidStringParam(path))
        throw InvalidArgsException("No path provided");

    if (!utils::isValidStringParam(key))
        throw InvalidArgsException("No key provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    const auto ddbPathStr = std::string(ddbPath);

    const auto ddb = ddb::open(ddbPathStr, true);
    auto json = ddb->getMetaManager()->get(std::string(key), std::string(path), ddbPathStr);

    utils::copyToPtr(json.dump(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBMetaUnset(const char* ddbPath, const char* path, const char* key, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidStringParam(path))
        throw InvalidArgsException("No path provided");

    if (!utils::isValidStringParam(key))
        throw InvalidArgsException("No key provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    const auto ddb = ddb::open(std::string(ddbPath), true);
    auto json =
        ddb->getMetaManager()->unset(std::string(key), std::string(path), std::string(ddbPath));

    utils::copyToPtr(json.dump(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBMetaList(const char* ddbPath, const char* path, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidStringParam(path))
        throw InvalidArgsException("No path provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    const auto ddbPathStr = std::string(ddbPath);

    const auto ddb = ddb::open(ddbPathStr, true);
    auto json = ddb->getMetaManager()->list(std::string(path), ddbPathStr);

    utils::copyToPtr(json.dump(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBMetaDump(const char* ddbPath, const char* ids, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidStringParam(ids))
        throw InvalidArgsException("No ids provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    json jIds;
    try {
        jIds = json::parse(ids);
    } catch (const json::parse_error& e) {
        throw InvalidArgsException(e.what());
    }

    const auto ddb = ddb::open(std::string(ddbPath), true);
    auto json = ddb->getMetaManager()->dump(jIds);

    utils::copyToPtr(json.dump(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBMetaRestore(const char* ddbPath, const char* dump, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidStringParam(dump))
        throw InvalidArgsException("No dump provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    json jDump;
    try {
        jDump = json::parse(dump);
    } catch (const json::parse_error& e) {
        throw InvalidArgsException(e.what());
    }

    const auto ddb = ddb::open(std::string(ddbPath), true);
    auto json = ddb->getMetaManager()->restore(jDump);

    utils::copyToPtr(json.dump(), output);

    DDB_C_END
}

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

DDB_DLL DDBErr DDBStac(const char* ddbPath,
                       const char* entry,
                       const char* stacCollectionRoot,
                       const char* id,
                       const char* stacCatalogRoot,
                       char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    // entry, stacCollectionRoot, id, stacCatalogRoot can be null/empty - they are optional
    // parameters
    const std::string entryStr = entry ? std::string(entry) : "";
    const std::string stacCollectionRootStr =
        stacCollectionRoot ? std::string(stacCollectionRoot) : "";
    const std::string idStr = id ? std::string(id) : "";
    const std::string stacCatalogRootStr = stacCatalogRoot ? std::string(stacCatalogRoot) : "";

    const auto ddb = ddb::open(std::string(ddbPath), false);
    auto json = ddb::generateStac(std::string(ddbPath),
                                  entryStr,
                                  stacCollectionRootStr,
                                  idStr,
                                  stacCatalogRootStr);

    utils::copyToPtr(json.dump(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBStacItemCollection(const char* ddbPath,
                                     const char* stacCollectionRoot,
                                     const char* id,
                                     const char* stacCatalogRoot,
                                     const char* bbox,
                                     const char* datetime,
                                     int limit,
                                     int offset,
                                     char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    const std::string stacCollectionRootStr =
        stacCollectionRoot ? std::string(stacCollectionRoot) : "";
    const std::string idStr = id ? std::string(id) : "";
    const std::string stacCatalogRootStr = stacCatalogRoot ? std::string(stacCatalogRoot) : "";

    const auto bboxVec = parseStacBbox(bbox);
    const auto datetimeRange = parseStacDatetime(datetime);

    auto json = ddb::generateStacItemCollection(std::string(ddbPath),
                                                stacCollectionRootStr,
                                                idStr,
                                                stacCatalogRootStr,
                                                bboxVec,
                                                datetimeRange.start,
                                                datetimeRange.end,
                                                limit,
                                                offset);

    utils::copyToPtr(json.dump(), output);

    DDB_C_END
}

DDBErr DDBStacData(const char* ddbPath, const char* entry, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    try {
        const auto stac =
            ddb::generateStacData(std::string(ddbPath), entry != nullptr ? entry : "");
        utils::copyToPtr(stac.dump(), output);
    } catch (const EntryNotFoundException& e) {
        DDBSetLastError(e.what());
        return DDBERR_NOTFOUND;
    }

    DDB_C_END
}

DDBErr DDBStacItemCollectionData(const char* ddbPath,
                                 const char* bbox,
                                 const char* datetime,
                                 int limit,
                                 int offset,
                                 char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    const auto datetimeRange = parseStacDatetime(datetime);
    const auto stac = ddb::generateStacItemCollectionData(std::string(ddbPath),
                                                          parseStacBbox(bbox),
                                                          datetimeRange.start,
                                                          datetimeRange.end,
                                                          limit,
                                                          offset);
    utils::copyToPtr(stac.dump(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBGetRasterInfo(const char* path, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(path))
        throw InvalidArgsException("No path provided");
    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    auto& spm = ddb::SensorProfileManager::instance();
    std::string jsonStr = spm.getRasterInfoJson(std::string(path));
    utils::copyToPtr(jsonStr, output);

    DDB_C_END
}

DDB_DLL DDBErr DDBGetRasterMetadata(const char* path, const char* formula,
                                     const char* bandFilter, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(path))
        throw InvalidArgsException("No path provided");
    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    const std::string formulaStr = formula ? std::string(formula) : "";
    const std::string filterStr = bandFilter ? std::string(bandFilter) : "";

    // Owned: the formula, statistics and sensor steps below can throw while it is open
    GDALDatasetUniquePtr dataset(GDALDataset::FromHandle(GDALOpen(path, GA_ReadOnly)));
    if (!dataset)
        throw GDALException("Cannot open " + std::string(path));
    GDALDatasetH hDs = GDALDataset::ToHandle(dataset.get());

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

    utils::copyToPtr(result.dump(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBGenerateMemoryThumbnailEx(const char* filePath, int size,
                                             const char* preset, const char* bands,
                                             const char* formula, const char* bandFilter,
                                             const char* colormap, const char* rescale,
                                             uint8_t** outBuffer, int* outBufferSize) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(filePath))
        throw InvalidArgsException("No file path provided");
    if (size < 0)
        throw InvalidArgsException("Invalid size parameter");
    if (outBuffer == nullptr)
        throw InvalidArgsException("Output buffer pointer is null");
    if (outBufferSize == nullptr)
        throw InvalidArgsException("Output buffer size pointer is null");

    ddb::ThumbVisParams visParams;
    if (preset) visParams.preset = std::string(preset);
    if (bands) visParams.bands = std::string(bands);
    if (formula) visParams.formula = std::string(formula);
    if (bandFilter) visParams.bandFilter = std::string(bandFilter);
    if (colormap) visParams.colormap = std::string(colormap);
    if (rescale) visParams.rescale = std::string(rescale);

    ddb::generateImageThumbEx(fs::path(filePath), size, "", outBuffer, outBufferSize, visParams);

    DDB_C_END
}

DDB_DLL DDBErr DDBMemoryTileEx(const char* inputPath,
                                int tz, int tx, int ty,
                                int tileSize, bool tms,
                                bool forceRecreate, const char* inputPathHash,
                                const char* preset, const char* bands,
                                const char* formula, const char* bandFilter,
                                const char* colormap, const char* rescale,
                                uint8_t** outBuffer, int* outBufferSize) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(inputPath))
        throw InvalidArgsException("No input path provided");
    if (outBuffer == nullptr)
        throw InvalidArgsException("Output buffer pointer is null");
    if (outBufferSize == nullptr)
        throw InvalidArgsException("Output buffer size pointer is null");
    if (tileSize < 0)
        throw InvalidArgsException("Invalid tile size parameter");
    if (tz < 0 || tx < 0 || ty < 0)
        throw InvalidArgsException("Invalid tile coordinates");

    // Build vizKey from params for cache differentiation
    std::string vizKey;
    if (preset) vizKey += std::string("p:") + preset;
    if (bands) vizKey += std::string(",b:") + bands;
    if (formula) vizKey += std::string(",f:") + formula;
    if (bandFilter) vizKey += std::string(",bf:") + bandFilter;
    if (colormap) vizKey += std::string(",cm:") + colormap;
    if (rescale) vizKey += std::string(",rs:") + rescale;

    const std::string hashStr = inputPathHash ? std::string(inputPathHash) : "";

    ddb::ThumbVisParams visParams;
    if (preset) visParams.preset = std::string(preset);
    if (bands) visParams.bands = std::string(bands);
    if (formula) visParams.formula = std::string(formula);
    if (bandFilter) visParams.bandFilter = std::string(bandFilter);
    if (colormap) visParams.colormap = std::string(colormap);
    if (rescale) visParams.rescale = std::string(rescale);

    ddb::TilerHelper::getTile(std::string(inputPath),
                              tz, tx, ty,
                              tileSize, tms,
                              forceRecreate,
                              "",
                              visParams,
                              outBuffer, outBufferSize,
                              hashStr + vizKey);

    DDB_C_END
}

DDB_DLL DDBErr DDBValidateMergeMultispectral(const char** paths, int numPaths, char** output) {
    DDB_C_BEGIN

    if (paths == nullptr || numPaths < 1)
        throw InvalidArgsException("No input paths provided");
    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    std::vector<std::string> inputPaths;
    for (int i = 0; i < numPaths; i++) {
        if (paths[i]) inputPaths.emplace_back(paths[i]);
    }

    auto validationResult = ddb::validateMergeMultispectral(inputPaths);

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

    utils::copyToPtr(j.dump(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBPreviewMergeMultispectral(const char** paths, int numPaths,
                                             const char* previewBands, int thumbSize,
                                             uint8_t** outBuffer, int* outBufferSize) {
    DDB_C_BEGIN

    if (paths == nullptr || numPaths < 1)
        throw InvalidArgsException("No input paths provided");
    if (outBuffer == nullptr)
        throw InvalidArgsException("Output buffer pointer is null");
    if (outBufferSize == nullptr)
        throw InvalidArgsException("Output buffer size pointer is null");

    std::vector<std::string> inputPaths;
    for (int i = 0; i < numPaths; i++) {
        if (paths[i]) inputPaths.emplace_back(paths[i]);
    }

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

    ddb::previewMergeMultispectral(inputPaths, bandsVec, thumbSize > 0 ? thumbSize : 512, outBuffer, outBufferSize);

    DDB_C_END
}

DDB_DLL DDBErr DDBMergeMultispectral(const char** paths, int numPaths, const char* outputPath) {
    DDB_C_BEGIN

    if (paths == nullptr || numPaths < 2)
        throw InvalidArgsException("At least 2 input paths required");
    if (utils::isNullOrEmptyOrWhitespace(outputPath))
        throw InvalidArgsException("No output path provided");

    std::vector<std::string> inputPaths;
    for (int i = 0; i < numPaths; i++) {
        if (paths[i]) inputPaths.emplace_back(paths[i]);
    }

    ddb::mergeMultispectral(inputPaths, std::string(outputPath));

    DDB_C_END
}

DDB_DLL DDBErr DDBValidateAlignRaster(const char* sourcePath,
                                      const char* referencePath,
                                      char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(sourcePath))
        throw InvalidArgsException("sourcePath is required");
    if (utils::isNullOrEmptyOrWhitespace(referencePath))
        throw InvalidArgsException("referencePath is required");
    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    auto r = ddb::validateAlignRaster(std::string(sourcePath),
                                      std::string(referencePath));
    utils::copyToPtr(ddb::alignValidationToJson(r), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBAlignRaster(const char* sourcePath,
                              const char* referencePath,
                              const char* outputPath,
                              const char* mode,
                              char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(sourcePath))
        throw InvalidArgsException("sourcePath is required");
    if (utils::isNullOrEmptyOrWhitespace(referencePath))
        throw InvalidArgsException("referencePath is required");
    if (utils::isNullOrEmptyOrWhitespace(outputPath))
        throw InvalidArgsException("outputPath is required");
    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

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
    utils::copyToPtr(ddb::alignResultToJson(r), output);

    DDB_C_END
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
    CPLSetThreadLocalConfigOption("COMPRESS_OVERVIEW", "DEFLATE");
    CPLSetThreadLocalConfigOption("PREDICTOR_OVERVIEW", "2");
    GDALBuildOverviews(hDs, "AVERAGE", static_cast<int>(levels.size()),
                       levels.data(), 0, nullptr, nullptr, nullptr);
    CPLSetThreadLocalConfigOption("COMPRESS_OVERVIEW", nullptr);
    CPLSetThreadLocalConfigOption("PREDICTOR_OVERVIEW", nullptr);
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

// Internal block-windowed raster export shared by DDBExportRaster (legacy ABI)
// and DDBExportRaster2. Returns 0 on success, 1 if canceled by the progress
// callback. Throws on errors (translated by the DDB_C_END macro in the callers).
static int exportRasterImpl(const char* inputPath, const char* outputPath,
                            const char* preset, const char* bands,
                            const char* formula, const char* bandFilter,
                            const char* colormap, const char* rescale,
                            int tileSize,
                            DDBProgressCallback progress, void* progressUserData) {
    if (utils::isNullOrEmptyOrWhitespace(inputPath))
        throw InvalidArgsException("No input path provided");
    if (utils::isNullOrEmptyOrWhitespace(outputPath))
        throw InvalidArgsException("No output path provided");
    // Creating the output truncates an existing file, so it must not be the input
    io::assureDistinctOutput(inputPath, outputPath);

    // Returns true if the operation should be canceled.
    auto reportProgress = [&](double fraction, const char* phase) -> bool {
        if (!progress) return false;
        return progress(fraction, phase, progressUserData) != 0;
    };

    GDALDatasetH hSrcDs = GDALOpen(inputPath, GA_ReadOnly);
    if (!hSrcDs)
        throw GDALException("Cannot open " + std::string(inputPath));

    const int width = GDALGetRasterXSize(hSrcDs);
    const int height = GDALGetRasterYSize(hSrcDs);
    const int bandCount = GDALGetRasterCount(hSrcDs);

    const std::string formulaStr = formula ? std::string(formula) : "";
    const std::string bandsStr = bands ? std::string(bands) : "";
    const std::string presetStr = preset ? std::string(preset) : "";
    const std::string filterStr = bandFilter ? std::string(bandFilter) : "";
    const std::string colormapStr = colormap ? std::string(colormap) : "";
    const std::string rescaleStr = rescale ? std::string(rescale) : "";

    // Get source geotransform and projection
    double geoTransform[6];
    bool hasGeoTransform = (GDALGetGeoTransform(hSrcDs, geoTransform) == CE_None);
    const char* projRef = GDALGetProjectionRef(hSrcDs);
    std::string projection = projRef ? std::string(projRef) : "";

    // RAII guard so the source dataset is always closed on any return or throw.
    auto gdalCloser = [](GDALDatasetH h) { if (h) GDALClose(h); };
    std::unique_ptr<std::remove_pointer_t<GDALDatasetH>, decltype(gdalCloser)>
        srcGuard(hSrcDs, gdalCloser);

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

        char** createOpts = nullptr;
        createOpts = CSLAddString(createOpts, "TILED=YES");
        createOpts = CSLAddString(createOpts, ("BLOCKXSIZE=" + std::to_string(tw)).c_str());
        createOpts = CSLAddString(createOpts, ("BLOCKYSIZE=" + std::to_string(th)).c_str());
        createOpts = CSLAddString(createOpts, "COMPRESS=DEFLATE");
        createOpts = CSLAddString(createOpts, "PREDICTOR=2");
        createOpts = CSLAddString(createOpts, "BIGTIFF=IF_SAFER");
        createOpts = CSLAddString(createOpts, "NUM_THREADS=ALL_CPUS");

        GDALDatasetH hOut = GDALCreate(gtiffDrv, outputPath, width, height, 4, GDT_Byte, createOpts);
        CSLDestroy(createOpts);
        if (!hOut)
            throw GDALException("Cannot create output GeoTIFF");

        // RAII guard for the output dataset: closed on any throw. On success or
        // cancel we release it explicitly after the appropriate finalization.
        std::unique_ptr<std::remove_pointer_t<GDALDatasetH>, decltype(gdalCloser)>
            outGuard(hOut, gdalCloser);

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

        reportProgress(0.0, "rendering");
        for (int ty = 0; ty < nty; ty++) {
            for (int tx = 0; tx < ntx; tx++) {
                if (reportProgress(static_cast<double>(doneTiles) / totalTiles, "rendering")) {
                    outGuard.reset();  // closes hOut
                    srcGuard.reset();  // closes hSrcDs
                    GDALDeleteDataset(gtiffDrv, outputPath);
                    return 1;
                }

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
        reportProgress(1.0, "rendering");
        return 0;
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

        // Use GDALTranslate for band selection + rescale.
        // Note: GDALTranslate does not expose an incremental progress callback
        // via its options API, so callers will only receive progress at 0.0 and
        // 1.0. Cancellation is only possible before the translate starts.
        if (reportProgress(0.0, "converting")) return 1;
        char** targs = nullptr;
        targs = CSLAddString(targs, "-ot");
        targs = CSLAddString(targs, "Byte");

        for (int b : selectedBands) {
            targs = CSLAddString(targs, "-b");
            targs = CSLAddString(targs, std::to_string(b).c_str());
        }

        GDALDataType srcType = GDALGetRasterDataType(GDALGetRasterBand(hSrcDs, 1));
        if (srcType != GDT_Byte) {
            if (!rescaleStr.empty()) {
                auto commaPos = rescaleStr.find(',');
                if (commaPos != std::string::npos) {
                    targs = CSLAddString(targs, "-scale");
                    targs = CSLAddString(targs, rescaleStr.substr(0, commaPos).c_str());
                    targs = CSLAddString(targs, rescaleStr.substr(commaPos + 1).c_str());
                    targs = CSLAddString(targs, "0");
                    targs = CSLAddString(targs, "255");
                }
            } else {
                targs = CSLAddString(targs, "-scale");
            }
        }

        targs = CSLAddString(targs, "-of");
        targs = CSLAddString(targs, "GTiff");
        targs = CSLAddString(targs, "-co");
        targs = CSLAddString(targs, "TILED=YES");
        targs = CSLAddString(targs, "-co");
        targs = CSLAddString(targs, "COMPRESS=DEFLATE");

        auto psOptions = GDALTranslateOptionsNew(targs, nullptr);
        CSLDestroy(targs);
        if (!psOptions)
            throw GDALException("Cannot create GDALTranslate options");

        int bUsageError = FALSE;
        GDALDatasetH hOut = GDALTranslate(outputPath, hSrcDs, psOptions, &bUsageError);
        GDALTranslateOptionsFree(psOptions);
        srcGuard.reset();  // closes hSrcDs

        if (!hOut || bUsageError) {
            if (hOut) GDALClose(hOut);
            throw GDALException("Cannot create output GeoTIFF");
        }

        GDALFlushCache(hOut);
        GDALClose(hOut);
        // Reopen in update mode to add internal overviews (efficient zoomed
        // reads). Best-effort: a failure here is non-fatal.
        GDALDatasetH hOvr = GDALOpen(outputPath, GA_Update);
        if (hOvr) {
            buildInternalOverviews(hOvr, width, height);
            GDALClose(hOvr);
        }
        reportProgress(1.0, "converting");
        return 0;
    }
}

DDB_DLL DDBErr DDBExportRaster(const char* inputPath, const char* outputPath,
                                const char* preset, const char* bands,
                                const char* formula, const char* bandFilter,
                                const char* colormap, const char* rescale) {
    DDB_C_BEGIN
    // Legacy ABI: forward to the block-windowed implementation with auto tile
    // size and no progress callback (so it can never be canceled).
    exportRasterImpl(inputPath, outputPath, preset, bands, formula, bandFilter,
                     colormap, rescale, 0, nullptr, nullptr);
    DDB_C_END
}

DDB_DLL DDBErr DDBExportRaster2(const char* inputPath, const char* outputPath,
                                 const char* preset, const char* bands,
                                 const char* formula, const char* bandFilter,
                                 const char* colormap, const char* rescale,
                                 int tileSize,
                                 DDBProgressCallback progress, void* progressUserData) {
    DDB_C_BEGIN
    const int canceled = exportRasterImpl(inputPath, outputPath, preset, bands,
                                          formula, bandFilter, colormap, rescale,
                                          tileSize, progress, progressUserData);
    if (canceled)
        return DDBERR_CANCELED;
    DDB_C_END
}


DDB_DLL DDBErr DDBGetRasterValueInfo(const char *filePath, char **output) {
    DDB_C_BEGIN
    if (utils::isNullOrEmptyOrWhitespace(filePath))
        throw InvalidArgsException("No file path provided");
    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    std::string jsonStr = ddb::getRasterValueInfoJson(std::string(filePath));
    utils::copyToPtr(jsonStr, output);
    DDB_C_END
}

DDB_DLL DDBErr DDBGetRasterPointValue(const char *filePath, int x, int y, char **output) {
    DDB_C_BEGIN
    if (utils::isNullOrEmptyOrWhitespace(filePath))
        throw InvalidArgsException("No file path provided");
    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    std::string jsonStr = ddb::getRasterPointJson(std::string(filePath), x, y);
    utils::copyToPtr(jsonStr, output);
    DDB_C_END
}

DDB_DLL DDBErr DDBGetRasterAreaStats(const char *filePath, int x0, int y0, int x1, int y1, char **output) {
    DDB_C_BEGIN
    if (utils::isNullOrEmptyOrWhitespace(filePath))
        throw InvalidArgsException("No file path provided");
    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    std::string jsonStr = ddb::getRasterAreaStatsJson(std::string(filePath), x0, y0, x1, y1);
    utils::copyToPtr(jsonStr, output);
    DDB_C_END
}

DDB_DLL DDBErr DDBGetRasterProfile(const char *filePath, const char *geoJsonLineString,
                                   int samples, char **output) {
    DDB_C_BEGIN
    if (utils::isNullOrEmptyOrWhitespace(filePath))
        throw InvalidArgsException("No file path provided");
    if (utils::isNullOrEmptyOrWhitespace(geoJsonLineString))
        throw InvalidArgsException("No GeoJSON LineString provided");
    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    std::string jsonStr = ddb::getRasterProfileJson(std::string(filePath),
                                                    std::string(geoJsonLineString),
                                                    samples);
    utils::copyToPtr(jsonStr, output);
    DDB_C_END
}

DDB_DLL DDBErr DDBCalculateVolume(const char *rasterPath,
                                  const char *polygonGeoJSON,
                                  const char *baseMethod,
                                  double flatElevation,
                                  char **output) {
    DDB_C_BEGIN
    if (utils::isNullOrEmptyOrWhitespace(rasterPath))
        throw InvalidArgsException("No raster path provided");
    if (utils::isNullOrEmptyOrWhitespace(polygonGeoJSON))
        throw InvalidArgsException("No polygon GeoJSON provided");
    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    const std::string method = (baseMethod == nullptr) ? std::string()
                                                       : std::string(baseMethod);
    std::string jsonStr = ddb::calculateVolumeJson(std::string(rasterPath),
                                                   std::string(polygonGeoJSON),
                                                   method,
                                                   flatElevation);
    utils::copyToPtr(jsonStr, output);
    DDB_C_END
}

DDB_DLL DDBErr DDBDetectStockpile(const char *rasterPath,
                                  double lat, double lon,
                                  double radius,
                                  float sensitivity,
                                  char **output) {
    DDB_C_BEGIN
    if (utils::isNullOrEmptyOrWhitespace(rasterPath))
        throw InvalidArgsException("No raster path provided");
    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    std::string jsonStr = ddb::detectStockpileJson(std::string(rasterPath),
                                                   lat, lon, radius, sensitivity);
    utils::copyToPtr(jsonStr, output);
    DDB_C_END
}

DDB_DLL DDBErr DDBDetectAllStockpiles(const char *rasterPath,
                                      float sensitivity,
                                      double minAreaM2,
                                      int maxResults,
                                      char **output) {
    DDB_C_BEGIN
    if (utils::isNullOrEmptyOrWhitespace(rasterPath))
        throw InvalidArgsException("No raster path provided");
    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    std::string jsonStr = ddb::detectAllStockpilesJson(std::string(rasterPath),
                                                       sensitivity,
                                                       minAreaM2,
                                                       maxResults);
    utils::copyToPtr(jsonStr, output);
    DDB_C_END
}

DDB_DLL DDBErr DDBGenerateContours(const char *rasterPath,
                                   double interval,
                                   int count,
                                   double baseOffset,
                                   double minElev,
                                   double maxElev,
                                   double simplifyTolerance,
                                   int bandIndex,
                                   char **output) {
    DDB_C_BEGIN
    if (utils::isNullOrEmptyOrWhitespace(rasterPath))
        throw InvalidArgsException("No raster path provided");
    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    ContourOptions opts;
    if (interval > 0.0) opts.interval = interval;
    if (count > 0) opts.count = count;
    opts.baseOffset = baseOffset;
    if (std::isfinite(minElev)) opts.minElev = minElev;
    if (std::isfinite(maxElev)) opts.maxElev = maxElev;
    opts.simplifyTolerance = std::max(0.0, simplifyTolerance);
    opts.bandIndex = (bandIndex > 0) ? bandIndex : 1;

    std::string jsonStr = ddb::generateContoursJson(std::string(rasterPath), opts);
    utils::copyToPtr(jsonStr, output);
    DDB_C_END
}

DDB_DLL DDBErr DDBMaskBorders(const char *input, const char *output, int nearDist, bool white) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(input))
        throw InvalidArgsException("No input path provided");
    if (utils::isNullOrEmptyOrWhitespace(output))
        throw InvalidArgsException("No output path provided");

    ddb::maskBorders(std::string(input), std::string(output), nearDist, white);

    DDB_C_END
}


// =====================================================================
// OGC services C API (raster region rendering + vector query/describe).
// =====================================================================

DDB_DLL DDBErr DDBRenderRasterRegion(const char *inputPath,
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

DDB_DLL DDBErr DDBRenderRasterRegionEx(const char *inputPath,
                                       const double *bbox,
                                       const char *bboxSrs,
                                       const char *outputCrs,
                                       const int *bands,
                                       int bandCount,
                                       int width, int height,
                                       const char *format,
                                       uint8_t **outBuffer,
                                       int *outBufferSize) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(inputPath))
        throw InvalidArgsException("No input path provided");
    if (bbox == nullptr)
        throw InvalidArgsException("No bbox provided");
    if (outBuffer == nullptr)
        throw InvalidArgsException("outBuffer is null");
    if (outBufferSize == nullptr)
        throw InvalidArgsException("outBufferSize is null");
    if (bandCount < 0)
        throw InvalidArgsException("bandCount is negative");
    if (bandCount > 0 && bands == nullptr)
        throw InvalidArgsException("bands is null but bandCount > 0");

    const std::string srs = bboxSrs ? bboxSrs : "";
    const std::string fmt = format  ? format  : "";
    const std::string oSrs = outputCrs ? outputCrs : "";
    std::vector<int> bandVec;
    if (bandCount > 0) bandVec.assign(bands, bands + bandCount);

    ddb::renderRasterRegion(std::string(inputPath), bbox, srs,
                            width, height, fmt, outBuffer, outBufferSize,
                            oSrs, bandVec);

    DDB_C_END
}

DDB_DLL DDBErr DDBRenderRasterIndex(const char *inputPath,
                                    const char *indexName,
                                    const double *bbox,
                                    const char *bboxSrs,
                                    int width, int height,
                                    const char *format,
                                    uint8_t **outBuffer,
                                    int *outBufferSize) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(inputPath))
        throw InvalidArgsException("No input path provided");
    if (utils::isNullOrEmptyOrWhitespace(indexName))
        throw InvalidArgsException("No indexName provided");
    if (bbox == nullptr)
        throw InvalidArgsException("No bbox provided");
    if (outBuffer == nullptr || outBufferSize == nullptr)
        throw InvalidArgsException("outBuffer/outBufferSize is null");

    const std::string srs = bboxSrs ? bboxSrs : "";
    const std::string fmt = format ? format : "";
    ddb::renderRasterIndex(std::string(inputPath), std::string(indexName),
                           bbox, srs, width, height, fmt,
                           outBuffer, outBufferSize);

    DDB_C_END
}

DDB_DLL DDBErr DDBQueryRasterPoint(const char *inputPath,
                                   double x, double y,
                                   const char *srs,
                                   char **output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(inputPath))
        throw InvalidArgsException("No input path provided");
    if (output == nullptr)
        throw InvalidArgsException("output is null");

    const std::string s = srs ? srs : "";
    const std::string j = ddb::queryRasterPoint(std::string(inputPath), x, y, s);
    utils::copyToPtr(j, output);

    DDB_C_END
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
    if (utils::isNullOrEmptyOrWhitespace(vectorPath))
        throw InvalidArgsException("No vector path provided");
    if (numFids < 0 || (numFids > 0 && fids == nullptr))
        throw InvalidArgsException("Invalid feature id list");

    VectorQuery query;
    query.layerName = layerName ? layerName : "";
    if (bbox != nullptr)
        query.bbox = std::array<double, 4>{bbox[0], bbox[1], bbox[2], bbox[3]};
    query.bboxSrs = bboxSrs ? bboxSrs : "";
    query.fids.assign(fids, fids + numFids);
    query.where = where ? where : "";
    return query;
}
}  // namespace

DDB_DLL DDBErr DDBQueryVector(const char *vectorPath,
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

DDB_DLL DDBErr DDBQueryVectorEx(const char *vectorPath,
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
    DDB_C_BEGIN

    if (output == nullptr)
        throw InvalidArgsException("output is null");

    auto query = vectorSelection(vectorPath, layerName, bbox, bboxSrs, fids, numFids, where);
    query.outputFormat = mapVectorMime(outputFormat);

    // Clamp maxFeatures to spec range [1, 10000]; 0 -> default 1000.
    int mf = maxFeatures;
    if (mf <= 0) mf = 1000;
    if (mf > 10000) mf = 10000;
    query.maxFeatures = mf;
    query.startIndex = startIndex < 0 ? 0 : startIndex;

    utils::copyToPtr(ddb::queryVector(std::string(vectorPath), query), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBCountVector(const char *vectorPath,
                              const char *layerName,
                              const double *bbox,
                              const char *bboxSrs,
                              const long long *fids,
                              int numFids,
                              const char *where,
                              long long *count) {
    DDB_C_BEGIN

    if (count == nullptr)
        throw InvalidArgsException("count is null");

    const auto query = vectorSelection(vectorPath, layerName, bbox, bboxSrs, fids, numFids, where);
    *count = ddb::countVectorFeatures(std::string(vectorPath), query);

    DDB_C_END
}

DDB_DLL DDBErr DDBDescribeVector(const char *vectorPath,
                                 const char *layerName,
                                 char **output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(vectorPath))
        throw InvalidArgsException("No vector path provided");
    if (output == nullptr)
        throw InvalidArgsException("output is null");

    const std::string layer = layerName ? layerName : "";
    const std::string j = ddb::describeVector(std::string(vectorPath), layer);
    utils::copyToPtr(j, output);

    DDB_C_END
}

DDB_DLL DDBErr DDBMakeCadPrj(const char *crs, char **output) {
    DDB_C_BEGIN

    if (crs == nullptr)
        throw InvalidArgsException("No coordinate system provided");
    if (output == nullptr)
        throw InvalidArgsException("output is null");

    utils::copyToPtr(ddb::cad::prjContent(crs), output);

    DDB_C_END
}
