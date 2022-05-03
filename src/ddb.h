/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DDB_H
#define DDB_H

#include "ddb_export.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DDB_LOG_ENV "DDB_LOG"
#define DDB_DEBUG_ENV "DDB_DEBUG"

#define DDB_FOLDER ".ddb"

enum DDBErr {
    DDBERR_NONE = 0, // No error
    DDBERR_EXCEPTION = 1, // Generic app exception
    DDBERR_BUILDDEPMISSING = 2
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

/** Retrieve a single entry from the index
 * @param ddbPath path to a DroneDB database (parent of ".ddb")
 * @param path path of the entry
 * @param output pointer to C-string where to store result (JSON)
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBGet(const char *ddbPath, const char *path, char **output);

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
 * @param output pointer to C-string where to store output (JSON). Output contains the new DDB properties.
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBChattr(const char* ddbPath, const char *attrsJson, char **output);

/** Generate thumbnail
 * @param filePath path of the input file
 * @param size size constraint of the thumbnail (width or height)
 * @param destPath path of the destination file * 
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBGenerateThumbnail(const char *filePath, int size, const char *destPath);

/** Generate memory thumbnail.
 * @param filePath path of the input file
 * @param size size constraint of the thumbnail (width or height)
 * @param outBuffer pointer to output buffer. The caller is responsible for destroying the buffer with DDBVSIFree.
 * @param outBufferSize output buffer size.
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBGenerateMemoryThumbnail(const char *filePath, int size, uint8_t **outBuffer, int *outBufferSize);

/** Free a buffer allocated by DDB
 * @param buffer pointer to buffer to be freed
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBVSIFree(uint8_t *buffer);

/** Generate image/orthophoto/EPT tiles
 * @param inputPath path to the input geoTIFF/EPT
 * @param tz zoom level
 * @param tx X coordinates
 * @param ty Y coordinates
 * @param outputTilePath output pointer to C-String where to store output tile path.
 * @param tileSize tile size in pixels
 * @param tms Generate TMS-style tiles instead of XYZ
 * @param forceRecreate ignore cache and always recreate the tile
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBTile(const char *inputPath, int tz, int tx, int ty, char **outputTilePath, int tileSize = 256, bool tms = false, bool forceRecreate = false);

/** Generate image/orthophoto/EPT tiles in memory
 * @param inputPath path to the input geoTIFF/EPT
 * @param tz zoom level
 * @param tx X coordinates
 * @param ty Y coordinates
 * @param outBuffer pointer to output buffer. The caller is responsible for destroying the buffer with DDBVSIFree.
 * @param outBufferSize output buffer size.
 * @param tileSize tile size in pixels
 * @param tms Generate TMS-style tiles instead of XYZ
 * @param forceRecreate ignore cache and always recreate the tile
 * @param inputPathHash Optional hash of the resource to tile (if available), allowing smarter decisions about file downloads for certain resource types.
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBMemoryTile(const char *inputPath, int tz, int tx, int ty, uint8_t **outBuffer, int *outBufferSize, int tileSize = 256, bool tms = false, bool forceRecreate = false, const char *inputPathHash = "");

/** Generate delta between two ddbs 
 * @param ddbSourceStamp JSON stamp of the source DroneDB database
 * @param ddbTargetStamp JSON stamp of the target DroneDB database
 * @param output pointer to C-string where to store result
 * @param format output format. One of: ["text", "json"]
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBDelta(const char *ddbSourceStamp, const char *ddbTargetStamp, char **output, const char *format);

/** Apply a delta on a database
 * @param delta JSON delta between databases
 * @param sourcePath path to source database (or path to partial files to be added)
 * @param ddbPath path to DroneDB database to which the delta should be applied
 * @param mergeStrategy merge strategy to use
 * @param sourceMetaDump meta dump (JSON) of source database (extracted with DDBMetaDump)
 * @param conflicts pointer to C-string where to store list of conflicts (if any) in JSON
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBApplyDelta(const char *delta, const char *sourcePath, char *ddbPath, int mergeStrategy, char *sourceMetaDump, char **conflicts);

/** Compute map of local files that are available for delta adds operations
 * @param delta JSON delta between databases
 * @param ddbPath path to DroneDB database to which the delta should be applied
 * @param hlDestFolder path to folder where to create hard links for available files (or "" to not create hard links)
 * @param output pointer to C-string where to store map of hashes (JSON)
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBComputeDeltaLocals(const char *delta, const char *ddbPath, const char *hlDestFolder, char **output);

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

/** Get the current database's stamp
 * @param ddbPath path to the source DroneDB database (parent of ".ddb")
 *  *  @param output pointer to C-string where to store result (JSON)
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBGetStamp(const char *ddbPath, char **output);

/** Move entry
 * @param ddbPath path to the source DroneDB database (parent of ".ddb")
 * @param source source entry path
 * @param dest dest entry path
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBMoveEntry(const char *ddbPath, const char *source, const char *dest);

/** Build
 * @param ddbPath path to the source DroneDB database (parent of ".ddb")
 * @param source source entry path (optional, if not specified: build all)
 * @param dest dest filesystem path (optional, if not specified: default path)
 * @param force rebuild if already existing (default false)
 * @param pendingOnly build only pending files (default false)
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBBuild(const char *ddbPath, const char *source = nullptr, const char *dest = nullptr, bool force = false, bool pendingOnly = false);

/** IsBuildable
 * @param ddbPath path to the source DroneDB database (parent of ".ddb")
 * @param path Entry path
 * @param isBuildable if the entry is buildable
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBIsBuildable(const char *ddbPath, const char *path, bool *isBuildable);

/** IsBuildPending
 * @param ddbPath path to the source DroneDB database (parent of ".ddb")
 * @param isBuildable if the entry is buildable
 * @return DDBERR_NONE on success, an error otherwise */
DDB_DLL DDBErr DDBIsBuildPending(const char *ddbPath, bool *isBuildPending);


/** Add metadata
 *  @param ddbPath path to the source DroneDB database (parent of ".ddb")
 *  @param path Entry path to associate metadata with (or "" to add metadata to the DroneDB database itself)
 *  @param key Metadata key
 *  @param data Metadata data (JSON)
 *  @param output pointer to C-string where to store result (JSON) */
DDB_DLL DDBErr DDBMetaAdd(const char *ddbPath, const char *path, const char *key, const char *data, char **output);

/** Set metadata
 *  @param ddbPath path to the source DroneDB database (parent of ".ddb")
 *  @param path Entry path to associate metadata with (or "" to add metadata to the DroneDB database itself)
 *  @param key Metadata key
 *  @param data Metadata data (JSON)
 *  @param output pointer to C-string where to store result (JSON) */
DDB_DLL DDBErr DDBMetaSet(const char *ddbPath, const char *path, const char *key, const char *data, char **output);

/** Remove metadata
 *  @param ddbPath path to the source DroneDB database (parent of ".ddb")
 *  @param id UUID of the metadata item to remove
 *  @param output pointer to C-string where to store result (JSON) */
DDB_DLL DDBErr DDBMetaRemove(const char *ddbPath, const char *id, char **output);

/** Get metadata
 *  @param ddbPath path to the source DroneDB database (parent of ".ddb")
 *  @param path Entry path to retrieve metadata from (or "" to get metadata from the DroneDB database itself)
 *  @param key Metadata key
 *  @param output pointer to C-string where to store result (JSON) */
DDB_DLL DDBErr DDBMetaGet(const char *ddbPath, const char *path, const char *key, char **output);

/** Unset metadata
 *  @param ddbPath path to the source DroneDB database (parent of ".ddb")
 *  @param path Entry path to unset metadata for (or "" to unset metadata for the DroneDB database itself)
 *  @param key Metadata key
 *  @param output pointer to C-string where to store result (JSON) */
DDB_DLL DDBErr DDBMetaUnset(const char *ddbPath, const char *path, const char *key, char **output);

/** List metadata
 *  @param ddbPath path to the source DroneDB database (parent of ".ddb")
 *  @param path Entry path to list metadata for (or "" to list metadata for the DroneDB database itself)
 *  @param output pointer to C-string where to store result (JSON) */
DDB_DLL DDBErr DDBMetaList(const char *ddbPath, const char *path, char **output);

/** Dump metadata
 *  @param ddbPath path to the source DroneDB database (parent of ".ddb")
 *  @param ids List of IDs to include in the dump (JSON array of strings) or "[]" to dump all
 *  @param output pointer to C-string where to store result (JSON) */
DDB_DLL DDBErr DDBMetaDump(const char *ddbPath, const char *ids, char **output);

/** Restore metadata
 *  @param ddbPath path to the source DroneDB database (parent of ".ddb")
 *  @param dump JSON dump generated with DDBMetaDump
 *  @param output pointer to C-string where to store result (JSON) */
DDB_DLL DDBErr DDBMetaRestore(const char *ddbPath, const char *dump, char **output);


#ifdef __cplusplus
}
#endif

#endif // DDB_H
