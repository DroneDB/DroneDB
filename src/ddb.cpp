/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "ddb.h"

#include "gdal_inc.h"
#include <passwordmanager.h>

#include "database.h"
#include "dbops.h"
#include "entry.h"
#include "exceptions.h"
#include "info.h"
#include "build.h"
#include "json.h"
#include "logger.h"
#include "mio.h"
#include "net.h"
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

void DDBRegisterProcess(bool verbose) {
    // Prevent multiple initializations
    if (initialized) {
        LOGD << "Called DDBRegisterProcess when already initialized";
        return;
    }

#ifndef WIN32
    // Windows does not let us change env vars for some reason
    // so this works only on Unix
    std::string projPaths =
        io::getExeFolderPath().string() + ":/usr/share/proj";
    setenv("PROJ_LIB", projPaths.c_str(), 1);
#endif

#if !defined(WIN32) && !defined(MAC_OSX)
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

    Database::Initialize();
    net::Initialize();
    GDALAllRegister();

    initialized = true;
}

const char* DDBGetVersion() { return APP_VERSION; }

DDBErr DDBInit(const char* directory, char** outPath) {
    DDB_C_BEGIN
    if (directory == nullptr)
        throw InvalidArgsException("No directory provided");

    if (outPath == nullptr) throw InvalidArgsException("No output provided");

    const std::string ddbDirPath = initIndex(directory);
    utils::copyToPtr(ddbDirPath, outPath);

    DDB_C_END
}

const char* DDBGetLastError() { return ddbLastError; }

void DDBSetLastError(const char* err) {
    strncpy(ddbLastError, err, 255);
    ddbLastError[254] = '\0';
}

DDBErr DDBAdd(const char* ddbPath, const char** paths, int numPaths,
              char** output, bool recursive) {
    DDB_C_BEGIN

    if (ddbPath == nullptr) throw InvalidArgsException("No directory provided");

    if (paths == nullptr || numPaths == 0)
        throw InvalidArgsException("No paths provided");

    if (output == nullptr) throw InvalidArgsException("No output provided");

    const auto db = ddb::open(std::string(ddbPath), true);
    const std::vector<std::string> pathList(paths, paths + numPaths);
    auto outJson = json::array();
    addToIndex(db.get(), ddb::expandPathList(pathList, recursive, 0),
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

    if (ddbPath == nullptr) throw InvalidArgsException("No directory provided");

    if (paths == nullptr || numPaths == 0)
        throw InvalidArgsException("No paths provided");

    const auto db = ddb::open(std::string(ddbPath), true);
    const std::vector<std::string> pathList(paths, paths + numPaths);

    removeFromIndex(db.get(), pathList);
    DDB_C_END
}

DDBErr DDBInfo(const char** paths, int numPaths, char** output,
               const char* format, bool recursive, int maxRecursionDepth,
               const char* geometry, bool withHash, bool stopOnError) {
    DDB_C_BEGIN

    if (format == nullptr || strlen(format) == 0)
        throw InvalidArgsException("No format provided");

    if (geometry == nullptr || strlen(geometry) == 0)
        throw InvalidArgsException("No format provided");

    if (paths == nullptr || numPaths == 0)
        throw InvalidArgsException("No paths provided");

    if (output == nullptr) throw InvalidArgsException("No output provided");

    const std::vector<std::string> input(paths, paths + numPaths);
    std::ostringstream ss;
    info(input, ss, format, recursive, maxRecursionDepth, geometry, withHash,
         stopOnError);
    utils::copyToPtr(ss.str(), output);
    DDB_C_END
}

DDBErr DDBList(const char* ddbPath, const char** paths, int numPaths,
               char** output, const char* format, bool recursive,
               int maxRecursionDepth) {
    DDB_C_BEGIN

    if (ddbPath == nullptr) throw InvalidArgsException("No ddb path provided");

    if (format == nullptr || strlen(format) == 0)
        throw InvalidArgsException("No format provided");

    if (paths == nullptr || numPaths == 0)
        throw InvalidArgsException("No paths provided");

    if (output == nullptr) throw InvalidArgsException("No output provided");

    const auto db = ddb::open(std::string(ddbPath), true);
    const std::vector<std::string> pathList(paths, paths + numPaths);

    std::ostringstream ss;
    listIndex(db.get(), pathList, ss, format, recursive, maxRecursionDepth);

    utils::copyToPtr(ss.str(), output);

    DDB_C_END
}

DDBErr DDBAppendPassword(const char* ddbPath, const char* password) {
    DDB_C_BEGIN

    if (ddbPath == nullptr) throw InvalidArgsException("No ddb path provided");

    if (password == nullptr || strlen(password) == 0)
        throw InvalidArgsException("No password provided");

    const auto db = ddb::open(std::string(ddbPath), true);

    PasswordManager manager(db.get());

    manager.append(std::string(password));

    DDB_C_END
}

DDBErr DDBVerifyPassword(const char* ddbPath, const char* password,
                         bool* verified) {
    DDB_C_BEGIN

    if (ddbPath == nullptr) throw InvalidArgsException("No ddb path provided");

    // We allow empty password verification
    if (password == nullptr) throw InvalidArgsException("No password provided");

    if (verified == nullptr)
        throw InvalidArgsException("Output parameter pointer is null");

    const auto db = ddb::open(std::string(ddbPath), true);

    PasswordManager manager(db.get());

    *verified = manager.verify(std::string(password));

    DDB_C_END
}

DDBErr DDBClearPasswords(const char* ddbPath) {
    DDB_C_BEGIN

    if (ddbPath == nullptr) throw InvalidArgsException("No ddb path provided");

    const auto db = ddb::open(std::string(ddbPath), true);

    PasswordManager manager(db.get());

    manager.clearAll();

    DDB_C_END
}

DDB_DLL DDBErr DDBStatus(const char* ddbPath, char** output) {
    DDB_C_BEGIN

    if (ddbPath == nullptr) throw InvalidArgsException("No ddb path provided");

    if (output == nullptr) throw InvalidArgsException("No output provided");

    const auto db = ddb::open(std::string(ddbPath), true);

    std::ostringstream ss;

    const auto cb = [&ss](ddb::FileStatus status, const std::string& string) {
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

DDBErr DDBChattr(const char* ddbPath, const char* attrsJson, char** output) {
    DDB_C_BEGIN
    const auto db = ddb::open(std::string(ddbPath), true);
    const json j = json::parse(attrsJson);
    db->chattr(j);
    utils::copyToPtr(db->getAttributes().dump(), output);

    DDB_C_END
}

DDBErr DDBGenerateThumbnail(const char* filePath, int size,
                            const char* destPath) {
    DDB_C_BEGIN

    const auto imagePath = fs::path(filePath);
    const auto thumbPath = fs::path(destPath);

    generateThumb(imagePath, size, thumbPath, true);

    DDB_C_END
}

DDB_DLL DDBErr DDBGenerateMemoryThumbnail(const char *filePath,
                                          int size,
                                          uint8_t **outBuffer,
                                          int *outBufferSize){
    DDB_C_BEGIN

    const auto imagePath = fs::path(filePath);

    generateThumb(imagePath, size, "", true, outBuffer, outBufferSize);

    DDB_C_END
}

DDBErr DDBVSIFree(uint8_t *buffer){
    DDB_C_BEGIN
    VSIFree(buffer);
    DDB_C_END
}

DDB_DLL DDBErr DDBTile(const char* inputPath, int tz, int tx, int ty,
                       char** outputTilePath, int tileSize, bool tms,
                       bool forceRecreate) {
    DDB_C_BEGIN
    const auto tilePath = ddb::TilerHelper::getFromUserCache(
        inputPath, tz, tx, ty, tileSize, tms, forceRecreate);
    utils::copyToPtr(tilePath.string(), outputTilePath);
    DDB_C_END
}

DDBErr DDBMemoryTile(const char *inputPath, int tz, int tx, int ty, uint8_t **outBuffer, int *outBufferSize, int tileSize, bool tms, bool forceRecreate, const char *inputPathHash){
    DDB_C_BEGIN
    ddb::TilerHelper::getTile(
        inputPath, tz, tx, ty, tileSize, tms, forceRecreate, "", outBuffer, outBufferSize, std::string(inputPathHash));
    DDB_C_END
}


DDBErr DDBDelta(const char* ddbSource, const char* ddbTarget, char** output,
                const char* format) {
    DDB_C_BEGIN

    if (ddbSource == nullptr)
        throw InvalidArgsException("No ddb source path provided");

    if (ddbTarget == nullptr)
        throw InvalidArgsException("No ddb path provided");

    if (format == nullptr || strlen(format) == 0)
        throw InvalidArgsException("No format provided");

    if (output == nullptr) throw InvalidArgsException("No output provided");

    const auto sourceDb = ddb::open(std::string(ddbSource), false);
    const auto targetDb = ddb::open(std::string(ddbTarget), false);

    std::ostringstream ss;
    delta(sourceDb.get(), targetDb.get(), ss, format);

    utils::copyToPtr(ss.str(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBSetTag(const char* ddbPath, const char* newTag) {
    DDB_C_BEGIN

    if (ddbPath == nullptr) throw InvalidArgsException("No ddb path provided");

    TagManager manager(ddbPath);

    manager.setTag(newTag);

    if (newTag == nullptr) throw InvalidArgsException("No tag provided");

    DDB_C_END
}

DDB_DLL DDBErr DDBGetTag(const char* ddbPath, char** outTag) {
    DDB_C_BEGIN

    if (ddbPath == nullptr) throw InvalidArgsException("No ddb path provided");
    if (outTag == nullptr) throw InvalidArgsException("No tag provided");

    TagManager manager(ddbPath);

    const auto tag = manager.getTag();

    utils::copyToPtr(tag, outTag);

    DDB_C_END
}

DDB_DLL DDBErr DDBGetLastSync(const char* ddbPath, const char* registry,
                              long long* lastSync) {
    DDB_C_BEGIN

    if (ddbPath == nullptr) throw InvalidArgsException("No ddb path provided");
    if (lastSync == nullptr)
        throw InvalidArgsException("No last sync provided");

    SyncManager manager(ddbPath);

    *lastSync = registry == nullptr || !strlen(registry)
                    ? manager.getLastSync()
                    : manager.getLastSync(registry);

    DDB_C_END
}

DDB_DLL DDBErr DDBSetLastSync(const char* ddbPath, const char* registry,
                              const long long lastSync) {
    DDB_C_BEGIN

    if (ddbPath == nullptr) throw InvalidArgsException("No ddb path provided");

    SyncManager manager(ddbPath);

    if (registry == nullptr || !strlen(registry))
        manager.setLastSync(lastSync);
    else
        manager.setLastSync(lastSync, registry);

    DDB_C_END
}

DDB_DLL DDBErr DDBMoveEntry(const char *ddbPath, const char *source, const char *dest) {

    DDB_C_BEGIN

    if (ddbPath == nullptr) throw InvalidArgsException("No ddb path provided");
    if (source == nullptr) throw InvalidArgsException("No source path provided");
    if (dest == nullptr) throw InvalidArgsException("No dest path provided");

    const auto ddb = ddb::open(std::string(ddbPath), true);

    moveEntry(ddb.get(), std::string(source), std::string(dest));

    DDB_C_END

}

DDB_DLL DDBErr DDBBuild(const char *ddbPath, const char *source, const char *dest, bool force) {

    DDB_C_BEGIN

    if (ddbPath == nullptr) throw InvalidArgsException("No ddb path provided");

    const auto ddbPathStr = std::string(ddbPath);

    const auto ddb = ddb::open(ddbPathStr, true);

    const auto destPath =
        dest == nullptr
            ? (fs::path(ddbPathStr) / DDB_FOLDER / DDB_BUILD_PATH).generic_string()
            : std::string(dest);

    // We dont use this at the moment
    std::ostringstream ss;

    if (source == nullptr)
    {
        buildAll(ddb.get(), destPath, ss, force);
    } else {
        build(ddb.get(), std::string(source), destPath, ss, force);
    }
    
    DDB_C_END
   
}

DDB_DLL DDBErr DDBIsBuildable(const char *ddbPath, const char *path, bool *isBuildable) {

    DDB_C_BEGIN

    if (ddbPath == nullptr) throw InvalidArgsException("No ddb path provided");
    if (path == nullptr) throw InvalidArgsException("No path provided");
    if (isBuildable == nullptr) throw InvalidArgsException("Buildable parameter is null");

    const auto ddbPathStr = std::string(ddbPath);

    const auto ddb = ddb::open(ddbPathStr, true);

    std::string subfolder;

    *isBuildable = ddb::isBuildable(ddb.get(), std::string(path), subfolder);
    
    DDB_C_END
   
}

DDBErr DDBMetaAdd(const char *ddbPath, const char *path, const char *key, const char *data, char **output){
    DDB_C_BEGIN

    if (ddbPath == nullptr) throw InvalidArgsException("No ddb path provided");
    if (path == nullptr) throw InvalidArgsException("No path provided");
    if (key == nullptr) throw InvalidArgsException("No key provided");
    if (data == nullptr) throw InvalidArgsException("No data provided");

    const auto ddbPathStr = std::string(ddbPath);

    const auto ddb = ddb::open(ddbPathStr, true);
    auto json = ddb->getMetaManager()->add(std::string(key), std::string(data), std::string(path), ddbPathStr);

    utils::copyToPtr(json.dump(), output);

    DDB_C_END
}

DDBErr DDBMetaSet(const char *ddbPath, const char *path, const char *key, const char *data, char **output){
    DDB_C_BEGIN

    if (ddbPath == nullptr) throw InvalidArgsException("No ddb path provided");
    if (path == nullptr) throw InvalidArgsException("No path provided");
    if (key == nullptr) throw InvalidArgsException("No key provided");
    if (data == nullptr) throw InvalidArgsException("No data provided");

    const auto ddbPathStr = std::string(ddbPath);

    const auto ddb = ddb::open(ddbPathStr, true);
    auto json = ddb->getMetaManager()->set(std::string(key), std::string(data), std::string(path), ddbPathStr);

    utils::copyToPtr(json.dump(), output);

    DDB_C_END
}

DDBErr DDBMetaRemove(const char *ddbPath, const char *id, char **output){
    DDB_C_BEGIN

    if (ddbPath == nullptr) throw InvalidArgsException("No ddb path provided");
    if (id == nullptr) throw InvalidArgsException("No id provided");

    const auto ddb = ddb::open(std::string(ddbPath), true);
    auto json = ddb->getMetaManager()->remove(std::string(id));

    utils::copyToPtr(json.dump(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBMetaGet(const char *ddbPath, const char *path, const char *key, char **output){
    DDB_C_BEGIN

    if (ddbPath == nullptr) throw InvalidArgsException("No ddb path provided");
    if (path == nullptr) throw InvalidArgsException("No path provided");
    if (key == nullptr) throw InvalidArgsException("No key provided");

    const auto ddbPathStr = std::string(ddbPath);

    const auto ddb = ddb::open(ddbPathStr, true);
    auto json = ddb->getMetaManager()->get(std::string(key), std::string(path), ddbPathStr);

    utils::copyToPtr(json.dump(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBMetaUnset(const char *ddbPath, const char *path, const char *key, char **output){
    DDB_C_BEGIN

    if (ddbPath == nullptr) throw InvalidArgsException("No ddb path provided");
    if (path == nullptr) throw InvalidArgsException("No path provided");
    if (key == nullptr) throw InvalidArgsException("No key provided");

    const auto ddb = ddb::open(std::string(ddbPath), true);
    auto json = ddb->getMetaManager()->unset(std::string(key), std::string(path), std::string(ddbPath));

    utils::copyToPtr(json.dump(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBMetaList(const char *ddbPath, const char *path, char **output){
    DDB_C_BEGIN

    if (ddbPath == nullptr) throw InvalidArgsException("No ddb path provided");
    if (path == nullptr) throw InvalidArgsException("No path provided");

    const auto ddbPathStr = std::string(ddbPath);

    const auto ddb = ddb::open(ddbPathStr, true);
    auto json = ddb->getMetaManager()->list(std::string(path), ddbPathStr);

    utils::copyToPtr(json.dump(), output);

    DDB_C_END
}

