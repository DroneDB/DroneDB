/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "ddb.h"

#include <gdal_priv.h>
#include <passwordmanager.h>

#include "database.h"
#include "dbops.h"
#include "entry.h"
#include "exceptions.h"
#include "info.h"
#include "json.h"
#include "logger.h"
#include "mio.h"
#include "net.h"
#include "status.h"
#include "syncmanager.h"
#include "tagmanager.h"
#include "thumbs.h"
#include "tiler.h"
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
        }
    };

    statusIndex(db.get(), cb);

    utils::copyToPtr(ss.str(), output);

    DDB_C_END
}

DDBErr DDBChattr(const char* ddbPath, const char* attrsJson, char** output) {
    DDB_C_BEGIN
    const auto db = ddb::open(std::string(ddbPath), true);
    json j = json::parse(attrsJson);
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

DDB_DLL DDBErr DDBTile(const char* geotiffPath, int tz, int tx, int ty,
                       char** outputTilePath, int tileSize, bool tms,
                       bool forceRecreate) {
    DDB_C_BEGIN
    const auto tilePath = ddb::TilerHelper::getFromUserCache(
        geotiffPath, tz, tx, ty, tileSize, tms, forceRecreate);
    utils::copyToPtr(tilePath.string(), outputTilePath);
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
