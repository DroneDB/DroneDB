/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "ddb.h"

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
#include "c_api_capabilities.h"
#include "c_api_util.h"
#include "cancellation.h"
#include "log_forwarding.h"
#include "database.h"
#include "databasepool.h"
#include "raster_cache.h"
#include "dbops.h"
#include "cad.h"
#include "delta.h"
#include "delta_apply.h"
#include "entry.h"
#include "entry_path.h"
#include "entry_types.h"
#include "exceptions.h"
#include "gdal_raii.h"
#include "vis_render.h"
#include "gtiff_options.h"
#include "hash.h"
#include "info.h"
#include "json.h"
#include "json_options.h"
#include "logger.h"
#include "mask.h"
#include "align.h"
#include "merge_multispectral.h"
#include "mio.h"
#include "parallel.h"
#include "passwordmanager.h"
#include "process_config.h"
#include "process_init.h"
#include "raster_export.h"
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
#include "entry_filters.h"
#include "vegetation.h"
#include "version.h"

using namespace ddb;
// Every export is one call of the mapper (c_api_util.h): guarded, guardedOutput or guardedBuffer.
// The template argument names the codes besides NONE and EXCEPTION it may return: none means
// kFrozenCodes (exports of 1.x), kNewExportCodes is for the exports added in 2.0.
using capi::codeBit;
using capi::entryPathArg;
using capi::entryPathList;
using capi::guarded;
using capi::guardedBuffer;
using capi::guardedOutput;
using capi::isNullOrEmptyOrWhitespace;
using capi::isValidNonEmptyStringParam;
using capi::isValidStringParam;
using capi::kNewExportCodes;
using capi::kFrozenCodes;
using capi::optEntryPathArg;
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

uint32_t DDBGetAbiVersion() {
    return (static_cast<uint32_t>(DDB_ABI_MAJOR) << 16) | static_cast<uint32_t>(DDB_ABI_MINOR);
}

const char* DDBGetCapabilities() {
    return capi::capabilities();
}

// ------------------------------------------------------------------ process configuration

DDBErr DDBConfigure(const char* configJson) {
    return guarded<kNewExportCodes>([&] { ProcessConfig::get().configure(parseJsonArg(configJson)); });
}

DDBErr DDBGetEffectiveConfig(char** output) {
    return guardedOutput<kNewExportCodes>(output, [&] {
        requireOutput(output);
        return ProcessConfig::get().effective();
    });
}

// ------------------------------------------------------------------ shutdown

// Once no call is in flight, DDBShutdown closes what the process keeps open between calls
DDBErr DDBShutdown(int timeoutMs) {
    return capi::shutdown(timeoutMs, &ddb::closeKeptResources);
}

// ------------------------------------------------------------------ cancellation

// DDBCancelToken is opaque to the caller: the library's cancel::Token
static cancel::Token* tokenOf(DDBCancelToken* token) noexcept {
    return reinterpret_cast<cancel::Token*>(token);
}

DDBErr DDBCreateCancelToken(DDBCancelToken** token) {
    return guardedOutput<kNewExportCodes>(token, [&] {
        requireOutput(token);
        return reinterpret_cast<DDBCancelToken*>(cancel::Token::create());
    });
}

DDBErr DDBCancel(DDBCancelToken* token) {
    // A host cancels its requests while it shuts down: allowed in any state
    return guarded<kNewExportCodes, capi::CallKind::Unrestricted>([&] {
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
    return guarded<kNewExportCodes, capi::CallKind::Setting>([&] { cancel::setThreadToken(tokenOf(token)); });
}

// ------------------------------------------------------------------ logging

static_assert(DDB_LOG_TRACE == logfwd::kTrace && DDB_LOG_DEBUG == logfwd::kDebug && DDB_LOG_INFO == logfwd::kInfo &&
                  DDB_LOG_WARNING == logfwd::kWarning && DDB_LOG_ERROR == logfwd::kError &&
                  DDB_LOG_FATAL == logfwd::kFatal,
              "DDBLogLevel and the levels of log_forwarding.h are the same");
static_assert(std::is_same_v<DDBLogCallback, logfwd::Callback>, "DDBLogCallback is log_forwarding's callback");

DDBErr DDBSetLogCallback(DDBLogCallback callback, int minLevel, void* userData) {
    return guarded<kNewExportCodes, capi::CallKind::Setting>([&] { logfwd::setCallback(callback, minLevel, userData); });
}

DDBErr DDBSetThreadOperation(uint64_t opId) {
    return guarded<kNewExportCodes, capi::CallKind::Setting>([&] { logfwd::setThreadOperation(opId); });
}

// ------------------------------------------------------------------ files and index

DDBErr DDBNormalizePath(const char* path, const char* reservedJson, char** output) {
    return guardedOutput<kNewExportCodes>(output, [&] {
        if (path == nullptr)
            throw InvalidArgsException("No path provided");
        requireOutput(output);

        const auto rules = ReservedRules::fromJson(reservedJson);
        return json(normalizeEntryPath(path, rules));
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
                       outJson.push_back(e);
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
    const auto relPath = io::Path(io::fromUtf8(p)).relativeTo(db->rootDirectory()).generic();
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
        return json(result);
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
            if (isNullOrEmptyOrWhitespace(f.path))
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
        return json(result);
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
        if (!isValidNonEmptyStringParam(format))
            throw InvalidArgsException("No format provided");

        if (!isValidNonEmptyStringParam(geometry))
            throw InvalidArgsException("No geometry provided");

        const auto input = requirePathList(paths, numPaths);
        requireOutput(output);

        if (maxRecursionDepth < 0)
            throw InvalidArgsException("Invalid max recursion depth");

        std::ostringstream ss;
        InfoOptions options;
        options.format = parseOutputFormat(format);
        options.recursive = recursive;
        options.maxRecursionDepth = maxRecursionDepth;
        options.geometry = geometry;
        options.withHash = withHash;
        options.stopOnError = stopOnError;
        info(input, ss, options);
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
        const auto entryPath = entryPathArg(path, "No path provided");

        const auto db = ddb::open(std::string(ddbPath), false);

        const auto entry = ddb::getEntryWithMeta(db.get(), entryPath);
        if (!entry)
            throw EntryNotFoundException("No entry " + entryPath);
        return json(*entry).dump();
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

        if (!isValidNonEmptyStringParam(format))
            throw InvalidArgsException("No format provided");

        const auto pathList = requirePathList(paths, numPaths);
        requireOutput(output);

        if (maxRecursionDepth < 0)
            throw InvalidArgsException("Invalid max recursion depth");

        const auto db = ddb::open(std::string(ddbPath), true);

        std::ostringstream ss;
        listIndex(db.get(), pathList, ss, parseOutputFormat(format, {OutputFormat::Text, OutputFormat::Json}), recursive, maxRecursionDepth);
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
    query.types = filters::parseEntryTypeList(types, /*allowDirectory=*/true);
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
    return guardedOutput<kNewExportCodes>(output, [&] {
        requireDdbPath(ddbPath);
        const auto opts = requireOptions(options, sizeof(DDBListEntriesOptions), "options");
        if (opts.reserved != 0)
            throw InvalidArgsException("options->reserved must be 0");
        requireOutput(output);

        const EntryQuery query = entryQuery(folder, opts.recursive, namePattern, types, opts.withDetails);
        const auto db = ddb::open(std::string(ddbPath), false);
        return listEntriesJson(db.get(), query, opts.withBuild);
    });
}

DDBErr DDBSearch(const char* ddbPath, const char* query, char** output, const char* format) {
    return guardedOutput(output, [&] {
        requireDdbPath(ddbPath);

        if (!isValidStringParam(query))
            throw InvalidArgsException("No query provided");

        if (!isValidNonEmptyStringParam(format))
            throw InvalidArgsException("No format provided");

        requireOutput(output);

        // The query is a pattern over entry paths: '*' is an ordinary character of the rule
        const auto pattern = optEntryPathArg(query, "query");

        const auto db = ddb::open(std::string(ddbPath), false);

        std::ostringstream ss;
        searchIndex(db.get(), pattern, ss, parseOutputFormat(format, {OutputFormat::Text, OutputFormat::Json}));
        return ss.str();
    });
}

DDBErr DDBAppendPassword(const char* ddbPath, const char* password) {
    return guarded([&] {
        requireDdbPath(ddbPath);

        if (!isValidNonEmptyStringParam(password))
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

        if (!isValidStringParam(attrsJson))
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

namespace {

// The tile (tz, tx, ty) of inputPath; inputPathHash (null or empty when unknown) names the local
// copy of a remote file
ddb::TileRequest tileRequest(const char* inputPath, int tz, int tx, int ty, int tileSize, bool tms,
                             bool forceRecreate, const char* inputPathHash = nullptr) {
    ddb::TileRequest request;
    request.source.path = pathArg(inputPath);
    request.source.tileSize = tileSize;
    request.source.tms = tms;
    request.source.forceRecreate = forceRecreate;
    request.source.pathHash = optStr(inputPathHash);
    request.tz = tz;
    request.tx = tx;
    request.ty = ty;
    return request;
}

}  // namespace

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

        const auto tilePath = ddb::TilerHelper::getFromUserCache(
            tileRequest(inputPath, tz, tx, ty, tileSize, tms, forceRecreate));
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
        auto request = tileRequest(inputPath, tz, tx, ty, tileSize, tms, forceRecreate, inputPathHash);
        request.outBuffer = buffer;
        request.outBufferSize = bufferSize;
        ddb::TilerHelper::getTile(request);
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

        auto request = tileRequest(inputPath, tz, tx, ty, tileSize, tms, forceRecreate, inputPathHash);
        request.format = fmtStr;
        request.outBuffer = buffer;
        request.outBufferSize = bufferSize;
        ddb::TilerHelper::getTile(request);
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

        if (!isValidNonEmptyStringParam(format))
            throw InvalidArgsException("No format provided");

        requireOutput(output, "No output provided");

        const json source = parseJsonArg(ddbSourceStamp);
        const json dest = parseJsonArg(ddbTargetStamp);

        std::ostringstream ss;
        delta(source, dest, ss, parseOutputFormat(format, {OutputFormat::Text, OutputFormat::Json}));
        return ss.str();
    });
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

    DeltaApplication application;
    application.sourcePath = pathArg(sourcePath);
    application.mergeStrategy = static_cast<MergeStrategy>(mergeStrategy);
    application.sourceMetaDump = parseJsonArg(sourceMetaDump);
    // The added files must match the hashes of the delta. A host that checked the uploaded
    // bytes itself says which (verifiedHashes); without it DDB_VERIFY_PUSH_HASHES=0 skips the
    // check of every file
    const bool explicitHashes = verifiedHashesJson != nullptr;
    if (explicitHashes)
        application.verifiedHashes = parseVerifiedHashes(parseJsonArg(verifiedHashesJson));
    application.verifyHashes = explicitHashes || verifyPushHashesByDefault();

    const auto ddb = ddb::open(std::string(ddbPath), false);
    std::stringstream ss;
    return json(applyDelta(d, ddb.get(), application, ss));
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
    return guardedOutput<kNewExportCodes>(conflicts, [&] {
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

        return json(computeDeltaLocals(d, ddb.get(), std::string(hlDestFolder)));
    });
}

DDBErr DDBSetTag(const char* ddbPath, const char* newTag) {
    return guarded([&] {
        requireText(ddbPath, "No ddb path provided");

        if (!isValidStringParam(newTag))
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
        const std::vector<EntryType> typeFilter = filters::parseEntryTypeList(types);
        const std::vector<std::string> extensionFilter = filters::parseExtensionList(extensions);

        const auto db = ddb::open(std::string(ddbPath), true);
        auto outJson = json::array();

        rescanIndex(db.get(), typeFilter, extensionFilter, stopOnError,
                    [&outJson](const Entry& e, bool success, const std::string& error) {
                        // A failed entry has its path only
                        json j = success ? json(e) : json{{"path", e.path}, {"error", error}};
                        j["success"] = success;
                        outJson.push_back(j);
                        return true; // continue processing
                    });
        return outJson;
    });
}

DDBErr DDBMoveEntry(const char* ddbPath, const char* source, const char* dest) {
    return guarded([&] {
        requireDdbPath(ddbPath);
        const auto from = entryPathArg(source, "No source path provided", "source path");
        const auto to = entryPathArg(dest, "No dest path provided", "dest path");

        const auto ddb = ddb::open(std::string(ddbPath), true);
        moveEntry(ddb.get(), from, to);
    });
}

// ------------------------------------------------------------------ builds

namespace {

// The options of DDBBuildEx: NULL and "" are none; otherwise a JSON object whose only member,
// "threads", is an integer from 1 to BuildOptions::MaxThreads (not 2.0, true or "2")
BuildOptions buildOptionsArg(const char* optionsJson) {
    if (optionsJson == nullptr || optionsJson[0] == '\0')
        return {};
    const json options = parseJsonArg(optionsJson);
    if (!options.is_object())
        throw InvalidArgsException("The build options must be a JSON object");
    rejectUnknownMembers(options, {"threads"}, "build option");

    const auto threads = options.find("threads");
    if (threads == options.end())
        return {};
    if (!threads->is_number_integer())
        throw InvalidArgsException(BuildOptions::threadsRule());
    // An unsigned value beyond int64 is refused like any value above the maximum
    const int64_t value = threads->is_number_unsigned()
                              ? static_cast<int64_t>(std::min<uint64_t>(threads->get<uint64_t>(), INT64_MAX))
                              : threads->get<int64_t>();
    return BuildOptions::withThreads(value);
}

}  // namespace

DDBErr DDBBuild(const char* ddbPath, const char* source, const char* dest, bool force, bool pendingOnly) {
    return DDBBuildEx(ddbPath, source, dest, force, pendingOnly, nullptr);
}

DDBErr DDBBuildEx(const char* ddbPath, const char* source, const char* dest, bool force, bool pendingOnly,
                  const char* optionsJson) {
    // The two build codes DDBBuild has always returned (1.x); a refused option is DDBERR_EXCEPTION
    // like the other arguments of DDBBuild
    constexpr auto kBuildCodes =
        kFrozenCodes | codeBit(DDBERR_BUILDDEPMISSING) | codeBit(DDBERR_BUILDINPROGRESS);
    return guarded<kBuildCodes>([&] {
        requireText(ddbPath, "No ddb path provided");
        const BuildOptions options = buildOptionsArg(optionsJson);

        // dest and source can be null (optional parameters); dest is a folder of the
        // filesystem, source an entry path
        const auto destPath = optStr(dest);
        const auto path = optEntryPathArg(source, "source path");

        const auto ddb = ddb::open(std::string(ddbPath), true);

        if (path.empty()) {
            if (pendingOnly)
                buildPending(ddb.get(), destPath, force, options);
            else
                buildAll(ddb.get(), destPath, force, options);
        } else {
            build(ddb.get(), path, destPath, force, options);
        }
    });
}

DDBErr DDBCleanup(const char* ddbPath, char** output) {
    return guardedOutput(output, [&] {
        requireText(ddbPath, "No ddb path provided");
        requireOutput(output);

        const auto ddb = ddb::open(std::string(ddbPath), true);
        return json(cleanupBuild(ddb.get(), ""));
    });
}

DDBErr DDBIsBuildable(const char* ddbPath, const char* path, bool* isBuildable) {
    return guardedOutput(isBuildable, [&] {
        requireDdbPath(ddbPath);

        if (!isValidStringParam(path))
            throw InvalidArgsException("No path provided");

        requireOutput(isBuildable, "Buildable parameter is null");

        const auto entryPath = optEntryPathArg(path);
        const auto ddb = ddb::open(std::string(ddbPath), true);
        return ddb::isBuildable(ddb.get(), entryPath);
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
        return json(ddb::getPendingBuildInfo(db.get()));
    });
}

DDBErr DDBIsBuildActive(const char* ddbPath, const char* path, bool* isBuildActive) {
    return guardedOutput(isBuildActive, [&] {
        requireDdbPath(ddbPath);
        const auto entryPath = entryPathArg(path, "No path provided");
        requireOutput(isBuildActive, "isBuildActive parameter is null");

        const auto ddb = ddb::open(std::string(ddbPath), true);
        return ddb::isBuildActive(ddb.get(), entryPath);
    });
}

DDBErr DDBIsBuildComplete(const char* ddbPath, const char* path, bool* isBuildComplete) {
    return guardedOutput(isBuildComplete, [&] {
        requireDdbPath(ddbPath);
        const auto entryPath = entryPathArg(path, "No path provided");
        requireOutput(isBuildComplete, "isBuildComplete parameter is null");

        const auto ddb = ddb::open(std::string(ddbPath), true);
        return ddb::isBuildComplete(ddb.get(), entryPath);
    });
}

DDBErr DDBGetBuildStatus(const char* ddbPath, const char** paths, int numPaths, char** output) {
    return guardedOutput(output, [&] {
        requireDdbPath(ddbPath);
        const auto pathList = entryPathList(paths, numPaths);
        requireOutput(output);

        const auto ddb = ddb::open(std::string(ddbPath), true);
        return json(ddb::getBuildStatus(ddb.get(), pathList));
    });
}

// ------------------------------------------------------------------ build manifest

DDBErr DDBGetEntryTypes(char** output) {
    return guardedOutput<kNewExportCodes>(output, [&] {
        requireOutput(output);
        return entryTypesJson();
    });
}

DDBErr DDBGetBuildArtifacts(const char* ddbPath, const char** paths, int numPaths, char** output) {
    return guardedOutput<kNewExportCodes>(output, [&] {
        requireDdbPath(ddbPath);
        const auto pathList = entryPathList(paths, numPaths);
        requireOutput(output);

        const auto db = ddb::open(std::string(ddbPath), false);
        return getBuildArtifactsJson(db.get(), pathList);
    });
}

DDBErr DDBResolveBuildFile(const char* ddbPath, const char* hash, const char* path, char** outPath) {
    return guardedOutput<kNewExportCodes>(outPath, [&] {
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
    return guardedOutput<kNewExportCodes>(output, [&] {
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
        if (!isValidStringParam(value))
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

        const auto entryPath = optEntryPathArg(path);
        const auto ddbPathStr = std::string(ddbPath);
        const auto ddb = ddb::open(ddbPathStr, true);
        return ddb->getMetaManager()->add(std::string(key),
                                          std::string(data),
                                          entryPath,
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

        const auto entryPath = optEntryPathArg(path);
        const auto ddbPathStr = std::string(ddbPath);
        const auto ddb = ddb::open(ddbPathStr, true);
        return ddb->getMetaManager()->set(std::string(key),
                                          std::string(data),
                                          entryPath,
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

        const auto entryPath = optEntryPathArg(path);
        const auto ddbPathStr = std::string(ddbPath);
        const auto ddb = ddb::open(ddbPathStr, true);
        return ddb->getMetaManager()->get(std::string(key), entryPath, ddbPathStr);
    });
}

DDBErr DDBMetaUnset(const char* ddbPath, const char* path, const char* key, char** output) {
    return guardedOutput(output, [&] {
        requireMetaArgs(ddbPath, {{path, "No path provided"}, {key, "No key provided"}}, output);

        const auto entryPath = optEntryPathArg(path);
        const auto ddb = ddb::open(std::string(ddbPath), true);
        return ddb->getMetaManager()->unset(std::string(key), entryPath, std::string(ddbPath));
    });
}

DDBErr DDBMetaList(const char* ddbPath, const char* path, char** output) {
    return guardedOutput(output, [&] {
        requireMetaArgs(ddbPath, {{path, "No path provided"}}, output);

        const auto entryPath = optEntryPathArg(path);
        const auto ddbPathStr = std::string(ddbPath);
        const auto ddb = ddb::open(ddbPathStr, true);
        return ddb->getMetaManager()->list(entryPath, ddbPathStr);
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
        const auto entryPath = optEntryPathArg(entry, "entry path");
        const auto ddb = ddb::open(std::string(ddbPath), false);
        return ddb::generateStac(std::string(ddbPath),
                                 entryPath,
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

        const auto query = ddb::parseStacItemQuery(optStr(bbox), optStr(datetime), limit, offset);
        return ddb::generateStacItemCollection(std::string(ddbPath),
                                               optStr(stacCollectionRoot),
                                               optStr(id),
                                               optStr(stacCatalogRoot),
                                               query);
    });
}

DDBErr DDBStacData(const char* ddbPath, const char* entry, char** output) {
    return guardedOutput<kNewExportCodes>(output, [&] {
        requireDdbPath(ddbPath);
        requireOutput(output);

        return ddb::generateStacData(std::string(ddbPath), optEntryPathArg(entry, "entry path"));
    });
}

DDBErr DDBStacItemCollectionData(const char* ddbPath,
                                 const char* bbox,
                                 const char* datetime,
                                 int limit,
                                 int offset,
                                 char** output) {
    return guardedOutput<kNewExportCodes>(output, [&] {
        requireDdbPath(ddbPath);
        requireOutput(output);

        const auto query = ddb::parseStacItemQuery(optStr(bbox), optStr(datetime), limit, offset);
        return ddb::generateStacItemCollectionData(std::string(ddbPath), query);
    });
}

// ------------------------------------------------------------------ rasters

DDBErr DDBGetRasterInfo(const char* path, char** output) {
    return guardedOutput(output, [&] {
        requireText(path, "No path provided");
        requireOutput(output);

        return ddb::getRasterInfoJson(std::string(path));
    });
}

DDBErr DDBGetRasterMetadata(const char* path, const char* formula,
                            const char* bandFilter, char** output) {
    return guardedOutput(output, [&] {
        requireText(path, "No path provided");
        requireOutput(output);

        return ddb::getRasterMetadataJson(std::string(path), optStr(formula), optStr(bandFilter));
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

// The bands of a merge preview, "1,2,3": fewer than three (or none) is the first three bands
std::vector<int> previewBandList(const char* previewBands) {
    std::vector<int> bands;
    if (previewBands != nullptr) {
        std::istringstream ss(previewBands);
        std::string token;
        while (std::getline(ss, token, ',')) {
            try {
                bands.push_back(std::stoi(token));
            } catch (const std::exception&) {
                throw InvalidArgsException("Invalid preview band value: '" + token + "'");
            }
        }
    }
    if (bands.size() < 3)
        bands = {1, 2, 3};
    return bands;
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

        // The visualization is part of the name of the local copy of a remote file
        auto request = tileRequest(inputPath, tz, tx, ty, tileSize, tms, forceRecreate, nullptr);
        request.source.pathHash = optStr(inputPathHash) + vizKey;
        request.visParams = visParams(preset, bands, formula, bandFilter, colormap, rescale);
        request.outBuffer = buffer;
        request.outBufferSize = bufferSize;
        ddb::TilerHelper::getTile(request);
    });
}

DDBErr DDBValidateMergeMultispectral(const char** paths, int numPaths, char** output) {
    return guardedOutput(output, [&] {
        if (paths == nullptr || numPaths < 1)
            throw InvalidArgsException("No input paths provided");
        requireOutput(output);

        return ddb::mergeValidationToJson(ddb::validateMergeMultispectral(nonNullPaths(paths, numPaths)));
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

        ddb::previewMergeMultispectral(inputPaths, previewBandList(previewBands), thumbSize > 0 ? thumbSize : 512,
                                       buffer, bufferSize);
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

// The export shared by DDBExportRaster (legacy ABI) and DDBExportRaster2: the C arguments in a
// request, and the C progress callback as the library's. Throws on errors (mapped to a DDBErr by
// guarded() in the callers), CanceledException when the progress callback asked to stop or the
// call was canceled.
static void exportRasterImpl(const char* inputPath, const char* outputPath,
                            const char* preset, const char* bands,
                            const char* formula, const char* bandFilter,
                            const char* colormap, const char* rescale,
                            int tileSize,
                            DDBProgressCallback progress, void* progressUserData) {
    requireText(inputPath, "No input path provided");
    requireText(outputPath, "No output path provided");

    ddb::RasterExport request;
    request.input = inputPath;
    request.output = outputPath;
    request.preset = optStr(preset);
    request.bands = optStr(bands);
    request.formula = optStr(formula);
    request.bandFilter = optStr(bandFilter);
    request.colormap = optStr(colormap);
    request.rescale = optStr(rescale);
    request.tileSize = tileSize;
    if (progress != nullptr) {
        request.progress = [progress, progressUserData](double fraction, const char* phase) {
            return progress(fraction, phase, progressUserData) != 0;
        };
    }
    ddb::exportRaster(request);
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
    // DDBExportRaster2 has always returned DDBERR_CANCELED (1.x)
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

namespace {

// The region of the C arguments (bbox is checked by the callers)
ddb::RegionRequest regionRequest(const char* inputPath, const double* bbox, const char* bboxSrs,
                                 int width, int height, const char* format) {
    ddb::RegionRequest request;
    request.inputPath = inputPath;
    std::copy(bbox, bbox + 4, request.bbox.begin());
    request.bboxSrs = optStr(bboxSrs);
    request.width = width;
    request.height = height;
    request.format = optStr(format);
    return request;
}

}  // namespace

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

        ddb::renderRasterRegion(regionRequest(inputPath, bbox, bboxSrs, width, height, format),
                                {optStr(outputCrs), bandVec}, buffer, bufferSize);
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

        ddb::renderRasterIndex(regionRequest(inputPath, bbox, bboxSrs, width, height, format),
                               std::string(indexName), buffer, bufferSize);
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
