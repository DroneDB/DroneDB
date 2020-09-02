/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DDB_H
#define DDB_H

#include "ddb_export.h"

#ifdef __cplusplus
extern "C" {
#endif

enum DDBErr {
    DDBERR_NONE = 0, // No error
    DDBERR_EXCEPTION = 1 // Generic app exception
};

#define DDB_C_BEGIN try {
#define DDB_C_END }catch(const AppException &e){ \
    DDBSetLastError(e.what()); \
    return DDBERR_EXCEPTION; \
} \
return DDBERR_NONE;

extern char ddbLastError[255];
void DDBSetLastError(const char *err);

/** Get the last error message
 * @return last error message */
DDB_DLL const char* DDBGetLastError();

/** This must be called as the very first function
 * of every DDB process/program
 * @param verbose whether the program should output log messages to stdout */
DDB_DLL void DDBRegisterProcess(bool verbose = false);

/** Get library version */
DDB_DLL const char* DDBGetVersion();

/** Initialize a DroneDB database
 * @param directory Path to directory where to initialize the database
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBInit(const char *directory, char **outPath = NULL);

/** Add one or more files to a DroneDB database
 * @param ddbPath path to a DroneDB database (parent of ".ddb")
 * @param paths array of paths to add to index
 * @param numPaths number of paths
 * @param recursive whether to recursively add folders
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBAdd(const char *ddbPath, const char **paths, int numPaths, bool recursive = false);

/** Remove one or more files to a DroneDB database
 * @param ddbPath path to a DroneDB database (parent of ".ddb")
 * @param paths array of paths to add to index
 * @param numPaths number of paths
 * @param recursive whether to recursively remove folders
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBRemove(const char *ddbPath, const char **paths, int numPaths, bool recursive = false);

/** Retrieve information about files 
 * @param paths array of paths to parse
 * @param numPaths number of paths
 * @param output pointer to C-string where to store result
 * @param format output format. One of: ["text", "json", "geojson"]
 * @param recursive whether to recursively scan folders
 * @param maxRecursionDepth limit the depth of recursion
 * @param geometry type of geometry to return when format is "geojson". One of: ["auto", "point" or "polygon"]
 * @param withHash whether to compute SHA256 hashes
 * @param stopOnError whether to stop on failure 
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBInfo(const char **paths, int numPaths, char** output, const char *format = "text", bool recursive = false, int maxRecursionDepth = 0,
                       const char *geometry = "auto", bool withHash = false, bool stopOnError = true);


#ifdef __cplusplus
}
#endif

#endif // DDB_H
