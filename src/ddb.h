/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DDB_H
#define DDB_H

#include "ddb_export.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DDB_LOG_ENV "DDB_LOG"
#define DDB_DEBUG_ENV "DDB_DEBUG"

#define DDB_FOLDER ".ddb"

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
DDB_DLL DDBErr DDBInit(const char *directory, char **outPath);

/** Add one or more files to a DroneDB database
 * @param ddbPath path to a DroneDB database (parent of ".ddb")
 * @param paths array of paths to add to index
 * @param numPaths number of paths
 * @param output pointer to C-string where to store output
 * @param recursive whether to recursively add folders
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBAdd(const char *ddbPath, const char **paths, int numPaths, char** output, bool recursive = false);

/** Remove one or more files to a DroneDB database
 * @param ddbPath path to a DroneDB database (parent of ".ddb")
 * @param paths array of paths to add to index
 * @param numPaths number of paths
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBRemove(const char *ddbPath, const char **paths, int numPaths);

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

/** List files inside index 
 * @param ddbPath path to a DroneDB database (parent of ".ddb")
 * @param paths array of paths to parse
 * @param numPaths number of paths
 * @param output pointer to C-string where to store result
 * @param format output format. One of: ["text", "json"]
 * @param recursive whether to recursively scan folders
 * @param maxRecursionDepth limit the depth of recursion
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBList(const char *ddbPath, const char **paths, int numPaths, char **output, const char *format, bool recursive = false, int maxRecursionDepth = 0);

/** Append password to database
 * @param ddbPath path to a DroneDB database (parent of ".ddb")
 * @param password password to append
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBAppendPassword(const char* ddbPath, const char *password);

/** Verify database password
 * @param ddbPath path to a DroneDB database (parent of ".ddb")
 * @param password password to verify
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBVerifyPassword(const char* ddbPath, const char* password, bool *verified);

/** Clear all database passwords
 * @param ddbPath path to a DroneDB database (parent of ".ddb")
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBClearPasswords(const char* ddbPath);

/** Show differences between index and filesystem
 * @param ddbPath path to a DroneDB database (parent of ".ddb")
 * @param output pointer to C-string where to store result
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBStatus(const char* ddbPath, char **output);

/** Changes database attributes
 * @param ddbPath path to a DroneDB database (parent of ".ddb")
 * @param attrsJson array of object attributes as a JSON string
 * @param output pointer to C-string where to store output (JSON). Output contains the new DDB metadata.
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBChattr(const char* ddbPath, const char *attrsJson, char **output);

/** Generate thumbnail
 * @param filePath path of the input file
 * @param size size constraint of the thumbnail (width or height)
 * @param destPath path of the destination file * 
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBGenerateThumbnail(const char *filePath, int size, const char *destPath);

/** Generate orthophoto tiles
 * @param geotiffPath path to the input geoTIFF
 * @param tz zoom level
 * @param tx X coordinates
 * @param ty Y coordinates
 * @param outputTilePath output pointer to C-String where to store output tile path.
 * @param tileSize tile size in pixels
 * @param tms Generate TMS-style tiles instead of XYZ
 * @param forceRecreate ignore cache and always recreate the tile
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBTile(const char *geotiffPath, int tz, int tx, int ty, char **outputTilePath, int tileSize = 256, bool tms = false, bool forceRecreate = false);

/** Generate delta between two ddbs 
 * @param ddbSource path to the source DroneDB database (parent of ".ddb")
 * @param ddbTarget path to the target DroneDB database (parent of ".ddb")
 * @param output pointer to C-string where to store result
 * @param format output format. One of: ["text", "json"]
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBDelta(const char *ddbSource, const char *ddbTarget, char **output, const char *format);

/** Sets dataset tag
 * @param ddbPath path to the source DroneDB database (parent of ".ddb")
 * @param newTag the dataset tag to set
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBSetTag(const char *ddbPath, const char *newTag);

/** Gets dataset tag
 * @param ddbPath path to the source DroneDB database (parent of ".ddb")
 * @param outTag output pointer to C-String where to store dataset tag
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBGetTag(const char *ddbPath, char **outTag);

/** Gets dataset tag
 * @param ddbPath path to the source DroneDB database (parent of ".ddb")
 * @param registry registry url
 * @param lastSync output pointer to time_t where to store last sync time
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBGetLastSync(const char *ddbPath, const char *registry, long long *lastSync);

/** Sets dataset tag
 * @param ddbPath path to the source DroneDB database (parent of ".ddb")
 * @param registry registry url
 * @param lastSync time_t last sync time
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBSetLastSync(const char *ddbPath, const char *registry, const long long lastSync);

#ifdef __cplusplus
}
#endif

#endif // DDB_H
