/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <gdal_priv.h>
#include "ddb.h"
#include "version.h"
#include "mio.h"
#include "logger.h"
#include "database.h"
#include "net.h"
#include "exceptions.h"
#include "utils.h"

using namespace ddb;

char ddbLastError[255];

void DDBRegisterProcess(int verbose){

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

// TODO: C++ function with C wrapper?
// or C wrapper all the way?
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

        utils::copyToPtr(ddbDirPath, outPath);
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
