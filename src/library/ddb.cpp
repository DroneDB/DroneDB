/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "ddb.h"

#include <cpr/cpr.h>

#include "../../vendor/segvcatch/segvcatch.h"
#include "build.h"
#include "database.h"
#include "dbops.h"
#include "delta.h"
#include "entry.h"
#include "exceptions.h"
#include "gdal_inc.h"
#include "info.h"
#include "json.h"
#include "logger.h"
#include "mio.h"
#include "passwordmanager.h"
#include "stac.h"
#include "status.h"
#include "syncmanager.h"
#include "tagmanager.h"
#include "thumbs.h"
#include "tilerhelper.h"
#include "utils.h"
#include "version.h"

using namespace ddb;

char ddbLastError[255];

// Could not be enough in a multi-threaded environment: check std::once_flag and
// std::call_once instead
static bool initialized = false;

void handleSegv() {
    throw ddb::AppException("Application encoutered a segfault");
}

void handleFpe() {
    throw ddb::AppException("Application encountered a floating point exception");
}

void DDBRegisterProcess(bool verbose) {
    // Prevent multiple initializations
    if (initialized) {
        LOGD << "Called DDBRegisterProcess when already initialized";
        return;
    }

#ifndef WIN32
// Windows does not let us change env vars for some reason
// so this works only on Unix
#ifdef __APPLE__
    std::string projPaths =
        io::getExeFolderPath().string() + ":/opt/homebrew/share/proj:/usr/local/share/proj";
#else
    std::string projPaths = io::getExeFolderPath().string() + ":/usr/share/ddb";
#endif

    setenv("PROJ_LIB", projPaths.c_str(), 1);
    setenv("PROJ_DATA", projPaths.c_str(), 1);
#endif

#if !defined(WIN32) && !defined(__APPLE__)
    try {
        std::locale("");  // Raises a runtime error if current locale is invalid
    } catch (const std::runtime_error&) {
        setenv("LC_ALL", "C", 1);
    }
#endif

#ifdef WIN32
    // Allow path.string() calls to work with Unicode filenames
    std::setlocale(LC_CTYPE, "en_US.UTF8");
#endif

    // Gets the environment variable to enable logging to file
    const auto logToFile = std::getenv(DDB_LOG_ENV) != nullptr;

    // Enable verbose logging if the environment variable is set
    verbose = verbose || std::getenv(DDB_DEBUG_ENV) != nullptr;

    init_logger(logToFile);
    if (verbose || logToFile) {
        set_logger_verbose();
    }

    GDALAllRegister();

    // Black magic to catch segfaults/fpes and throw
    // C++ exceptions instead
    segvcatch::init_segv(&handleSegv);
    segvcatch::init_fpe(&handleFpe);

    initialized = true;
}

const char* DDBGetVersion() {
    return APP_VERSION;
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

DDBErr DDBGet(const char* ddbPath, const char* path, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (utils::isNullOrEmptyOrWhitespace(path))
        throw InvalidArgsException("No path provided");

    const auto db = ddb::open(std::string(ddbPath), false);

    auto entries = ddb::getMatchingEntries(db.get(), std::string(path));
    std::string entryJson;
    if (entries.size() == 1) {
        entryJson = entries[0].toJSONString();
    } else if (entries.size() > 1) {
        throw InvalidArgsException("Multiple entries were returned for " + std::string(path));
    } else {
        throw InvalidArgsException("No entry " + std::string(path));
    }
    utils::copyToPtr(entryJson, output);

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
        throw InvalidArgsException("Invalid size parameter");    const auto imagePath = fs::path(filePath);
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

    if (buffer == nullptr)
        throw InvalidArgsException("Buffer pointer is null");

    VSIFree(buffer);
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

    utils::copyToPtr("", outputTilePath);
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

    const auto ddb = ddb::open(std::string(ddbPath), false);
    std::stringstream ss;
    const auto adc = applyDelta(d,
                                fs::path(std::string(sourcePath)),
                                ddb.get(),
                                static_cast<MergeStrategy>(mergeStrategy),
                                metaDump,
                                ss);

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
    utils::copyToPtr(ddb->getStamp().dump(), output);

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
        return DDBERR_BUILDDEPMISSING;
    }

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

    // entry, stacCollectionRoot, id, stacCatalogRoot can be null/empty - they are optional parameters
    const std::string entryStr = entry ? std::string(entry) : "";
    const std::string stacCollectionRootStr = stacCollectionRoot ? std::string(stacCollectionRoot) : "";
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
