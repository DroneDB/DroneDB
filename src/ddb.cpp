/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <gdal_priv.h>
#include "ddb.h"
#include "dbops.h"
#include "info.h"
#include "version.h"
#include "mio.h"
#include "logger.h"
#include "database.h"
#include "net.h"
#include "exceptions.h"
#include "utils.h"

using namespace ddb;

char ddbLastError[255];

void DDBRegisterProcess(bool verbose){

#ifndef WIN32
    // Windows does not let us change env vars for some reason
    // so this works only on Unix
    std::string projPaths = io::getExeFolderPath().string() + ":/usr/share/proj";
    setenv("PROJ_LIB", projPaths.c_str(), 1);
#endif
    init_logger();
    if (verbose) {
        set_logger_verbose();
    }
    Database::Initialize();
    net::Initialize();
    GDALAllRegister();
}

const char* DDBGetVersion(){
    return APP_VERSION;
}

DDBErr DDBInit(const char *directory, char **outPath){
DDB_C_BEGIN
    fs::path dirPath = directory;
    if (!fs::exists(dirPath)) throw FSException("Invalid directory: " + dirPath.string()  + " (does not exist)");

    fs::path ddbDirPath = dirPath / ".ddb";
    if (std::string(directory) == ".") ddbDirPath = ".ddb"; // Nicer to the eye
    fs::path dbasePath = ddbDirPath / "dbase.sqlite";

    LOGD << "Checking if .ddb directory exists...";
    if (fs::exists(ddbDirPath)) {
        throw FSException("Cannot initialize database: " + ddbDirPath.string() + " already exists");
    } else {
        if (fs::create_directory(ddbDirPath)) {
            LOGD << ddbDirPath.string() + " created";
        } else {
            throw FSException("Cannot create directory: " + ddbDirPath.string() + ". Check that you have the proper permissions?");
        }
    }

    LOGD << "Checking if dbase exists...";
    if (fs::exists(dbasePath)) {
        throw FSException(ddbDirPath.string() + " already exists");
    } else {
        LOGD << "Creating " << dbasePath.string();

        // Create database
        std::unique_ptr<Database> db = std::make_unique<Database>();
        db->open(dbasePath.string());
        db->createTables();
        db->close();

        utils::copyToPtr(ddbDirPath.string(), outPath);
    }
DDB_C_END
}

const char *DDBGetLastError(){
    return ddbLastError;
}

void DDBSetLastError(const char *err){
    strncpy(ddbLastError, err, 255);
    ddbLastError[254] = '\0';
}

DDBErr DDBAdd(const char *ddbPath, const char **paths, int numPaths, bool recursive){
DDB_C_BEGIN
    auto db = ddb::open(std::string(ddbPath), true);
    std::vector<std::string> pathList(paths, paths + numPaths);
    ddb::addToIndex(db.get(), ddb::expandPathList(pathList,
                                                  recursive,
                                                  0));
DDB_C_END
}

DDBErr DDBRemove(const char *ddbPath, const char **paths, int numPaths, bool recursive){
DDB_C_BEGIN
    auto db = ddb::open(std::string(ddbPath), true);
    std::vector<std::string> pathList(paths, paths + numPaths);
    ddb::removeFromIndex(db.get(), ddb::expandPathList(pathList,
                                                  recursive,
                                                  0));
DDB_C_END
}

DDBErr DDBInfo(const char **paths, int numPaths, char **output, const char *format, bool recursive, int maxRecursionDepth, const char *geometry, bool withHash, bool stopOnError){
DDB_C_BEGIN
    std::vector<std::string> input(paths, paths + numPaths);
    std::ostringstream ss;
    ddb::info(input, ss, format, recursive, maxRecursionDepth,
              geometry, withHash, stopOnError);
    utils::copyToPtr(ss.str(), output);
DDB_C_END
}
