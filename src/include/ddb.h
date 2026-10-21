/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DDB_H
#define DDB_H

#include "ddb_export.h"
#include <stddef.h>
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

/* ddb.h is valid C11 as well as C++ (XI-03): tests/unit/c11_header.c includes it with -std=c11
 * -pedantic -Werror (MSVC: /std:c11 /W4 /WX). No default arguments, (void) for an empty
 * parameter list, bool from <stdbool.h> (1 byte: marshal it as U1), typedef'd types. */

#ifdef __cplusplus
#define DDB_STATIC_ASSERT(condition, message) static_assert(condition, message)
extern "C"
{
#else
#define DDB_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#endif

#define DDB_LOG_ENV "DDB_LOG"
#define DDB_DEBUG_ENV "DDB_DEBUG"

#define DDB_FOLDER ".ddb"

/* ABI version of this header (XI-01). The MINOR grows with every additive change (a new export,
 * a new error code, a new field in a JSON output); the MAJOR grows when an export is removed or
 * changes its signature or meaning. The exported DDB* symbols are listed in ddb.symbols, next to
 * this header: CI fails when the library exports a different list, or when the list changes
 * without a version bump (scripts/check-abi.py). A caller checks DDBGetAbiVersion() against the
 * version it was built for: the same MAJOR and a MINOR at least as large. */
#define DDB_ABI_MAJOR 1
#define DDB_ABI_MINOR 1

    /* Error codes. A new code is also listed in DDBGetCapabilities() (kErrorCodes, ddb.cpp).
     * Every code but DDBERR_NONE comes with a message (DDBGetLastError). An export returns the
     * codes its documentation names, DDBERR_NONE and DDBERR_EXCEPTION. In ABI 1.1 the exports
     * that existed before it keep the codes they always returned: an invalid argument and a
     * retryable failure are DDBERR_EXCEPTION there; DDBERR_INVALIDARGS and DDBERR_RETRYABLE are
     * returned by the exports added in ABI 1.1 only. */
    typedef enum DDBErr
    {
        DDBERR_NONE = 0,            // No error
        DDBERR_EXCEPTION = 1,       // Generic app exception
        DDBERR_BUILDDEPMISSING = 2, // Build skipped: dependency missing
        DDBERR_BUILDINPROGRESS = 3, // Build skipped: another process holds the lock
        DDBERR_CANCELED = 4,        // Operation canceled by the progress callback
        DDBERR_BUSY = 5,            // Transient DB contention (SQLITE_BUSY/LOCKED); safe to retry
        DDBERR_NOTFOUND = 6,        // The requested entry does not exist
        DDBERR_INVALIDARGS = 7,     // An argument is invalid: fix the call, do not retry (ABI 1.1)
        DDBERR_RETRYABLE = 8,       // A transient failure (DDBGetLastErrorReason): retry later (ABI 1.1)
        DDBERR_SHUTDOWN = 9         // The library is shut down (DDBShutdown): no call is accepted (ABI 1.1)
    } DDBErr;

    DDB_STATIC_ASSERT(sizeof(bool) == 1, "bool is 1 byte (marshal it as U1)");
    DDB_STATIC_ASSERT(sizeof(DDBErr) == 4, "DDBErr is 4 bytes");

    /** Progress callback for long-running operations.
     * @param fraction progress in [0,1], or -1 for indeterminate
     * @param phase short label of the current phase (may be NULL)
     * @param userData opaque pointer passed through from the caller
     * @return 0 to continue, non-zero to request cancellation */
    typedef int (*DDBProgressCallback)(double fraction, const char *phase, void *userData);

    /* The last error (XI-02) belongs to the calling thread. Every call clears it on entry and
     * sets it when it returns a code other than DDBERR_NONE, except the accessors that never
     * touch it: DDBGetLastError, DDBGetLastErrorReason, DDBGetVersion, DDBGetAbiVersion,
     * DDBGetCapabilities, DDBFree and DDBVSIFree. Read it on the thread that made the failing
     * call, before any other libddb call on that thread (no await or thread hop in between).
     * Outputs are written only on DDBERR_NONE: on another code an output pointer is left
     * untouched (or set to NULL). */

    /** The message of the last failed call on this thread, whole (UTF-8): never NULL, "" when
     * the last call succeeded. The string is owned by the library and stays valid until the
     * next libddb call on the same thread. */
    DDB_DLL const char *DDBGetLastError(void);

    /** The machine-readable reason of the last failed call on this thread, next to its message
     * (ABI 1.1): never NULL, "" when the failure has no reason or the last call succeeded.
     * Valid like DDBGetLastError(). A reason is a stable camelCase name; names are only ever
     * added (DDBGetCapabilities() lists them as "errorReasons"), so treat an unknown one like "".
     * Defined today, for a transient failure (the disk or a tool, not the input) after which the
     * same call can succeed later:
     *   "lowDiskSpace"     free disk space fell below the build budget
     *   "noSpaceOnDevice"  a write failed because the disk is full
     *   "idleTimeout"      an external tool stopped producing output and was stopped
     * An export added in ABI 1.1 returns DDBERR_RETRYABLE for them; an older export (DDBBuild)
     * returns DDBERR_EXCEPTION with the same reason. */
    DDB_DLL const char *DDBGetLastErrorReason(void);

    /** @deprecated (ABI 1.1): libddb sets and clears the last error itself. Replaces the last
     * error message of this thread with err (NULL clears it) and clears the reason. */
    DDB_DLL void DDBSetLastError(const char *err);

    /** This must be called as the very first function of every DDB process/program, before any
     * other call but the exempt accessors. A failed initialization is final: this and every
     * later call return DDBERR_EXCEPTION with its message (ABI 1.1; returned void before).
     * @param verbose whether the program should output log messages to stdout
     * @return DDBERR_NONE on success, DDBERR_EXCEPTION when the initialization failed */
    DDB_DLL DDBErr DDBRegisterProcess(bool verbose);

    /** Get library version */
    DDB_DLL const char *DDBGetVersion(void);

    /** ABI version of the loaded library: (DDB_ABI_MAJOR << 16) | DDB_ABI_MINOR.
     * Callable at any time, before DDBRegisterProcess too; it never changes DDBGetLastError(). */
    DDB_DLL uint32_t DDBGetAbiVersion(void);

    /** What the loaded library supports, as a JSON object:
     *   "abi"          {"major": DDB_ABI_MAJOR, "minor": DDB_ABI_MINOR}
     *   "version"      DDBGetVersion()
     *   "errorCodes"   every DDBErr code by name without the prefix: {"NONE": 0, "EXCEPTION": 1, ...}
     *   "errorReasons" every reason DDBGetLastErrorReason() returns, sorted
     *   "cancellable"  the exports whose DDBProgressCallback can cancel them (DDBERR_CANCELED)
     *   "symbols"      every exported DDB* function, sorted (ddb.symbols)
     * Fields are only ever added. The string is built once and owned by the library: it stays
     * valid for the life of the process and is the same pointer on every call and every thread;
     * do not free it. Callable at any time, before DDBRegisterProcess too; it never changes
     * DDBGetLastError().
     * @return the JSON, or NULL only if the process ran out of memory while building it */
    DDB_DLL const char *DDBGetCapabilities(void);

    /* Process configuration (DQ-15, XI-06, ABI 1.1). One set of settings for the whole process,
     * which a host sets with DDBConfigure and reads back with DDBGetEffectiveConfig. Each setting
     * takes, by precedence: the DDBConfigure value, else its environment variable, else the
     * built-in default. An environment variable that is not a valid value is ignored.
     *
     *   "cpuPoolSize"    integer 0..4096, env DDB_CPU_POOL_SIZE, default cores - 1.
     *                    The process-wide CPU token pool: the worker threads libddb may run at
     *                    once beside the calling threads, across every call (parallel indexing,
     *                    GDAL NUM_THREADS, PDAL readers, the cores given to Obj2Tiles and those
     *                    counted for untwine). A calling thread always works and never needs a
     *                    token; a section gets the tokens that are free and never waits for one,
     *                    so 0 runs every call on its calling thread only. The calls a host runs
     *                    at once plus cpuPoolSize bound libddb's busy CPU threads.
     *   "ioThreads"      integer 1..256, env DDB_IO_THREADS, default 4.
     *                    The threads of one disk-bound section (hashing the files of a delta,
     *                    deleting build folders); they take no CPU token.
     *   "minFreeDiskMb"  integer 0..10^13, env DDB_MIN_FREE_DISK_MB, default 512.
     *                    The free space in MiB a build or a copy of build artifacts needs on the
     *                    volume it writes to; below it the call fails with the reason
     *                    "lowDiskSpace". 0 turns the check off.
     *   "cacheDir"       absolute folder path (UTF-8), default the profile folder: DDB_HOME/.ddb
     *                    when DDB_HOME is set (source "environment"), else the home folder's .ddb.
     *                    Where libddb caches thumbnails, tiles, index templates and DSM tiles
     *                    (created when first needed). The registry credentials of the ddb command
     *                    line stay in the profile folder.
     *
     * The per-section thread counts DDB_INDEX_THREADS, DDB_BUILD_THREADS, DDB_RENDER_THREADS and
     * DDB_OBJ2TILES_MAX_CORES (environment or GDAL config options) stay what a section asks for;
     * the CPU pool decides how many of them run. */

    /** Changes settings of the process configuration (see above). Callable at any time, from any
     * thread, before DDBRegisterProcess too and concurrently with any other call; a host
     * normally calls it once, before DDBRegisterProcess. A change applies to the work that reads
     * the setting after this call returned: a parallel section that is running keeps the tokens
     * it holds (after the pool shrank, given-back tokens are handed out again only below the new
     * size), a call that is running keeps the cache folder it opened, and files in a previous
     * cache folder are neither moved nor deleted.
     * @param configJson a JSON object with the settings to change, by name: a value sets the
     *                   setting, null puts it back to its environment variable or default;
     *                   settings not named keep their value. "{}" changes nothing.
     * @return DDBERR_NONE; DDBERR_INVALIDARGS when configJson is NULL, not a JSON object, names an
     *         unknown setting or has an invalid value (see DDBGetLastError). An invalid call
     *         changes nothing: every member is checked before any is applied. An unknown name
     *         is an error, not ignored, so a misspelled budget never goes unnoticed; a host
     *         finds the names a library knows in DDBGetEffectiveConfig's "settings". */
    DDB_DLL DDBErr DDBConfigure(const char *configJson);

    /** The process configuration in effect (see above), as a JSON object:
     *   "settings"         every setting by name, with its value (an object DDBConfigure accepts)
     *   "sources"          every setting by name: "configure", "environment" or "default"
     *   "hardwareThreads"  the cores libddb sees (at least 1)
     * Fields and settings are only ever added. Callable at any time, like DDBConfigure.
     * @param output pointer to C-string where to store the JSON (free with DDBFree)
     * @return DDBERR_NONE; DDBERR_INVALIDARGS when output is NULL; DDBERR_EXCEPTION when the
     *         default cache folder cannot be found (no DDB_HOME, HOME or USERPROFILE) */
    DDB_DLL DDBErr DDBGetEffectiveConfig(char **output);

    /** Normalize a dataset-relative entry path, or reject it, with libddb's path rules (XI-09,
     * entry_path.h; golden vectors in tests/contract/vectors/entry-paths.json):
     * '/' and '\' both separate segments; empty and "." segments are dropped; the result uses '/'
     * with no leading or trailing separator. Names are compared byte for byte, without Unicode
     * normalization. ".ddb" is reserved as a segment at any depth.
     * The rejection codes, checked in this order: "empty" (""), "nul" (unreachable through this
     * NUL-terminated API), "encoding" (not valid UTF-8), "absolute" (leading '/' or '\', a UNC
     * path, a drive "X:"), "traversal" (a ".." segment), "empty" (nothing left, e.g. "./"),
     * "reserved". New codes may be added; treat an unknown code as a rejection.
     * The existing exports do not apply these rules yet (ABI 1.1).
     * @param path the path (UTF-8)
     * @param reservedJson NULL, "" or a JSON array of extra names reserved as the FIRST segment
     *                     only, e.g. [".uploads"]; each name is one segment (no separator, not
     *                     "", "." or "..")
     * @param output pointer to C-string where to store the JSON (free with DDBFree):
     *               {"path": "a/b"} when accepted, {"rejected": "<code>"} when rejected
     * @return DDBERR_NONE for an accepted and for a rejected path; DDBERR_INVALIDARGS when path
     *         or output is NULL or reservedJson is invalid (see DDBGetLastError) */
    DDB_DLL DDBErr DDBNormalizePath(const char* path, const char* reservedJson, char** output);

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
    DDB_DLL DDBErr DDBAdd(const char *ddbPath, const char **paths, int numPaths, char **output, bool recursive);

    /** Options for DDBAddWithOptions. Caller must
     * initialize every field explicitly; these are semantic defaults, not enforced ones. */
    typedef struct {
        bool recursive;
        bool stopOnError;        // default true == DDBAdd's semantics
        int maxConflictRetries;  // default 2
    } DDBAddOptions;

    DDB_STATIC_ASSERT(sizeof(DDBAddOptions) == 8, "DDBAddOptions layout");
    DDB_STATIC_ASSERT(offsetof(DDBAddOptions, recursive) == 0, "DDBAddOptions layout");
    DDB_STATIC_ASSERT(offsetof(DDBAddOptions, stopOnError) == 1, "DDBAddOptions layout");
    DDB_STATIC_ASSERT(offsetof(DDBAddOptions, maxConflictRetries) == 4, "DDBAddOptions layout");

    /** Add one or more files to a DroneDB database with per-item error isolation.
     * @param ddbPath path to a DroneDB database (parent of ".ddb")
     * @param paths array of paths to add to index
     * @param numPaths number of paths
     * @param options add behaviour; when options->stopOnError is false, a corrupt/unreadable
     *                file fails only itself instead of aborting the whole batch
     * @param output pointer to C-string where to store output JSON:
     *               { "entries": [...], "unchanged": [...], "errors": [...] } — every input
     *               path appears in exactly one of the three arrays
     * @return DDBERR_NONE on success (even if options->stopOnError is false and some items
     *         failed — check the "errors" array), an error otherwise */
    DDB_DLL DDBErr DDBAddWithOptions(const char *ddbPath, const char **paths, int numPaths,
                                     const DDBAddOptions *options, char **output);

    /** A file with the SHA-256 its writer computed while writing it (see DDBAddWithHints) */
    typedef struct {
        const char *path;   // file to add
        const char *sha256; // lowercase hex digest; NULL or "" = no hint, the file is hashed
        long long size;     // bytes written; < 0 = not checked
        double mtime;       // modification time after the write (seconds since the epoch); <= 0 = not checked
    } DDBAddHint;

    /* 64-bit layout (the one Registry marshals) */
    DDB_STATIC_ASSERT(sizeof(void *) != 8 || sizeof(DDBAddHint) == 32, "DDBAddHint layout");
    DDB_STATIC_ASSERT(sizeof(void *) != 8 || offsetof(DDBAddHint, sha256) == 8, "DDBAddHint layout");
    DDB_STATIC_ASSERT(sizeof(void *) != 8 || offsetof(DDBAddHint, size) == 16, "DDBAddHint layout");
    DDB_STATIC_ASSERT(sizeof(void *) != 8 || offsetof(DDBAddHint, mtime) == 24, "DDBAddHint layout");

    /** DDBAddWithOptions for files whose SHA-256 the caller computed while writing them.
     * A hint is used only when the file still has its size and mtime; otherwise the file is
     * hashed as usual, so a wrong hint can never index a wrong hash for a changed file.
     * Folders are not expanded (options->recursive is ignored).
     * @param files array of files with their hints
     * @param numFiles number of files
     * @param options, output as DDBAddWithOptions
     * @return as DDBAddWithOptions */
    DDB_DLL DDBErr DDBAddWithHints(const char *ddbPath, const DDBAddHint *files, int numFiles,
                                   const DDBAddOptions *options, char **output);

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
    DDB_DLL DDBErr DDBInfo(const char **paths, int numPaths, char **output, const char *format, bool recursive, int maxRecursionDepth,
                           const char *geometry, bool withHash, bool stopOnError);

    /** Close the idle pooled index connections and the idle cached rasters (tilers and
     *  datasets kept open between tile/render calls) of the files under a folder.
     *  Call it before deleting, moving or replacing a dataset folder (or its .ddb/dbase.sqlite)
     *  from this process: idle connections and rasters keep their files open.
     * @param path dataset folder (parent of ".ddb"), or a folder that contains datasets
     * @param closed out (optional): number of connections and rasters closed
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBReleaseConnections(const char *path, int *closed);

    /** Move the committed changes of an index from its WAL into the database file and empty
     *  the WAL. Index writes leave their changes in .ddb/dbase.sqlite-wal (no checkpoint per
     *  call): call this before copying dbase.sqlite without its WAL.
     * @param ddbPath path to a DroneDB database (parent of ".ddb")
     * @param complete out (optional): 1 when the WAL was emptied, 0 when another connection
     *        (a reader of an older snapshot, a writer) kept the checkpoint from completing
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBCheckpoint(const char *ddbPath, int *complete);

    /** Retrieve a single entry from the index, including its metadata
     * @param ddbPath path to a DroneDB database (parent of ".ddb")
     * @param path dataset-relative path of the entry (exact match, no wildcards)
     * @param output pointer to C-string where to store result (JSON)
     * @return DDBERR_NONE on success, DDBERR_NOTFOUND if no entry has this path, an error otherwise */
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
    DDB_DLL DDBErr DDBList(const char *ddbPath, const char **paths, int numPaths, char **output, const char *format, bool recursive, int maxRecursionDepth);

    /** List the entries below a folder that match a file name pattern and entry types. The
     * filters run in the index query, so non-matching entries are never read or serialized.
     * @param ddbPath path to a DroneDB database (parent of ".ddb")
     * @param folder dataset-relative folder, "" for the dataset root; a file selects itself
     * @param recursive the whole subtree of the folder (true) or its children only (false)
     * @param namePattern file name pattern ('*' any characters, '?' one character, letters in any
     *                    case: Unicode simple case folding since ABI 1.1, ASCII only before; a
     *                    name or pattern that is not valid UTF-8 folds ASCII only), or "" for
     *                    every name
     * @param types comma-separated entry types (see DDBRescan, "directory" allowed), or "" for all
     * @param withDetails true: full entries as DDBList's "json" format; false: path, hash, type,
     *                    mtime, size and depth only
     * @param output pointer to C-string where to store the JSON array, sorted by path
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBListEntries(const char *ddbPath, const char *folder, bool recursive, const char *namePattern,
                                  const char *types, bool withDetails, char **output);

    /** Search files inside index
     * @param ddbPath path to a DroneDB database (parent of ".ddb")
     * @param query search string
     * @param output pointer to C-string where to store result
     * @param format output format. One of: ["text", "json"]
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBSearch(const char *ddbPath, const char *query, char **output, const char *format);

    /** Append password to database
     * @param ddbPath path to a DroneDB database (parent of ".ddb")
     * @param password password to append
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBAppendPassword(const char *ddbPath, const char *password);

    /** Verify database password
     * @param ddbPath path to a DroneDB database (parent of ".ddb")
     * @param password password to verify
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBVerifyPassword(const char *ddbPath, const char *password, bool *verified);

    /** Clear all database passwords
     * @param ddbPath path to a DroneDB database (parent of ".ddb")
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBClearPasswords(const char *ddbPath);

    /** Show differences between index and filesystem
     * @param ddbPath path to a DroneDB database (parent of ".ddb")
     * @param output pointer to C-string where to store result
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBStatus(const char *ddbPath, char **output);

    /** @deprecated: Changes database attributes
     * @param ddbPath path to a DroneDB database (parent of ".ddb")
     * @param attrsJson array of object attributes as a JSON string
     * @param output pointer to C-string where to store output (JSON). Output contains the new DDB properties.
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBChattr(const char *ddbPath, const char *attrsJson, char **output);

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

    /** Free a string buffer allocated by a DDB function (via copyToPtr / calloc).
     * @param ptr pointer to string to be freed. If NULL, this is a no-op.
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBFree(char *ptr);

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
    DDB_DLL DDBErr DDBTile(const char *inputPath, int tz, int tx, int ty, char **outputTilePath, int tileSize, bool tms, bool forceRecreate);

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
    DDB_DLL DDBErr DDBMemoryTile(const char *inputPath, int tz, int tx, int ty, uint8_t **outBuffer, int *outBufferSize, int tileSize, bool tms, bool forceRecreate, const char *inputPathHash);

    /** Generate a tile in memory with explicit output format: "png", "jpeg" or "webp"
     *  (or their MIME types). PNG tiles are written at zlib level 1, WebP tiles lossy at
     *  quality 80; tiles without transparent pixels have no alpha band. */
    DDB_DLL DDBErr DDBMemoryTileFmt(const char *inputPath, int tz, int tx, int ty, uint8_t **outBuffer, int *outBufferSize, int tileSize, bool tms, bool forceRecreate, const char *inputPathHash, const char *outputFormat);

    /** Generate delta between two ddbs
     * @param ddbSourceStamp JSON stamp of the source DroneDB database
     * @param ddbTargetStamp JSON stamp of the target DroneDB database
     * @param output pointer to C-string where to store result
     * @param format output format. One of: ["text", "json"]
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBDelta(const char *ddbSourceStamp, const char *ddbTargetStamp, char **output, const char *format);

    /** Apply a delta on a database. The added files are moved out of sourcePath, and each
     * must match the hash in the delta (checked before any change; set the environment
     * variable DDB_VERIFY_PUSH_HASHES=0 to skip the check)
     * @param delta JSON delta between databases
     * @param sourcePath path to source database (or path to partial files to be added)
     * @param ddbPath path to DroneDB database to which the delta should be applied
     * @param mergeStrategy merge strategy to use
     * @param sourceMetaDump meta dump (JSON) of source database (extracted with DDBMetaDump)
     * @param conflicts pointer to C-string where to store list of conflicts (if any) in JSON
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBApplyDelta(const char *delta, const char *sourcePath, const char *ddbPath, int mergeStrategy, const char *sourceMetaDump, char **conflicts);

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

    /** Get the checksum of the current database's stamp (the "checksum" field of
     * DDBGetStamp) without building the stamp
     * @param ddbPath path to the source DroneDB database (parent of ".ddb")
     * @param output pointer to C-string where to store the checksum (hex SHA-256)
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBGetStampChecksum(const char *ddbPath, char **output);

    /** Rescan all files in the index to update metadata
     * @param ddbPath path to a DroneDB database (parent of ".ddb")
     * @param output pointer to C-string where to store output (JSON array of results)
     * @param types comma-separated list of entry types to rescan (e.g., "image,geoimage,pointcloud"), or empty for all.
     *              Valid types: generic, geoimage, georaster, pointcloud, image, dronedb, markdown, video, geovideo, model, panorama, geopanorama, vector, gaussiansplat
     * @param stopOnError whether to stop processing on first error (default true)
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBRescan(const char *ddbPath, char **output, const char *types, bool stopOnError);

    /** Rescan the files in the index that match both a type and an extension filter
     * @param ddbPath path to a DroneDB database (parent of ".ddb")
     * @param output pointer to C-string where to store output (JSON array of results, same format as DDBRescan)
     * @param types comma-separated list of entry types to rescan (see DDBRescan), or empty for all
     * @param extensions comma-separated list of file extensions to rescan (e.g., "dwg,.dxf,copc.laz"; case and
     *                   leading dot are ignored), or empty for all
     * @param stopOnError whether to stop processing on first error (default true)
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBRescanEx(const char *ddbPath, char **output, const char *types,
                               const char *extensions, bool stopOnError);

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
     * @param pendingOnly retry only pending (deferred) builds (default false). Each due
     *        entry (last attempt at least 60 s ago, or force) gets a full build attempt;
     *        per-entry failures are logged, not returned.
     * @return DDBERR_NONE on success, DDBERR_BUILDDEPMISSING when a single-entry build is
     *         deferred because a companion file is missing (a ".pending" marker is
     *         written), an error otherwise. Missing external tools and dependency
     *         references that can never be satisfied (absolute, or outside the dataset)
     *         are errors, not deferrals. */
    DDB_DLL DDBErr DDBBuild(const char *ddbPath, const char *source, const char *dest, bool force, bool pendingOnly);

    /** Cleanup a dataset:
     *   1. Remove from the database all (non-directory) entries whose underlying
     *      file no longer exists on the filesystem.
     *   2. Remove orphaned build artifacts whose hash no longer corresponds to
     *      any entry. Subdirectories with an active (live-PID) build lock are
     *      skipped.
     * @param ddbPath path to a DroneDB database (parent of ".ddb")
     * @param output pointer to C-string with a JSON object of the form
     *               { "entries": [...], "builds": [...] } listing what was
     *               removed (caller must free with DDBFree)
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBCleanup(const char *ddbPath, char **output);

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

    /** Get information about pending (deferred) builds, i.e. builds skipped
     *  because one or more companion files were missing at the time of the
     *  attempt (names are dataset-root-relative paths). This is a read-only view: it
     *  does not consume ".pending" marker files or trigger any build.
     * @param ddbPath path to the source DroneDB database (parent of ".ddb")
     * @param output pointer to C-string where to store result (JSON array).
     *               Each element: { "hash": "...", "path": "...",
     *               "missingDependencies": ["..."], "lastAttempt": 1721612345 }
     *               (caller must free with DDBFree)
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBGetPendingBuildInfo(const char *ddbPath, char **output);

    /** IsBuildActive
     * @param ddbPath path to the source DroneDB database (parent of ".ddb")
     * @param path Entry path to check if build is active for this entry
     * @param isBuildActive if a build is currently active for this entry
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBIsBuildActive(const char *ddbPath, const char *path, bool *isBuildActive);

    /** IsBuildComplete
     *  Reports whether the build artifacts for the given entry exist and have
     *  non-zero content. Returns false for non-buildable entries, missing
     *  outputs, or empty stub folders left by interrupted builds.
     * @param ddbPath path to the source DroneDB database (parent of ".ddb")
     * @param path Entry path to check
     * @param isBuildComplete out: true if build output is complete and valid
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBIsBuildComplete(const char *ddbPath, const char *path, bool *isBuildComplete);

    /** GetBuildStatus
     *  DDBIsBuildable and DDBIsBuildComplete for several entries with one index open.
     * @param ddbPath path to the source DroneDB database (parent of ".ddb")
     * @param paths array of dataset-relative entry paths (exact match)
     * @param numPaths number of paths
     * @param output pointer to C-string where to store result (JSON array, one element per
     *               input path, in input order). Each element:
     *               { "path": "...", "exists": true, "buildable": true, "complete": false }
     *               (caller must free with DDBFree)
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBGetBuildStatus(const char *ddbPath, const char **paths, int numPaths, char **output);

    /* ---- Build manifest (XI-04, ABI 1.1) -------------------------------------------------------
     * libddb owns the build layout, the build state and the entry type rules: a host serves the
     * roles, paths and states these exports return, and never composes a build path, names an
     * artifact file or keeps its own list of buildable types.
     *
     * Artifact roles say what an artifact is for, never where it is. They are only ever added:
     * pass an unknown role through, and ignore it where it has no use.
     *   raster.cog       Cloud Optimized GeoTIFF of a GeoRaster (downloadable)
     *   vector.gpkg      GeoPackage of a Vector (downloadable)
     *   vector.mvt       Mapbox Vector Tiles of a Vector: a directory, its index the TileJSON
     *   vector.local     drawing-coordinate metadata of a CAD drawing without a CRS (it has no MVT)
     *   pointcloud.copc  Cloud Optimized Point Cloud (downloadable)
     *   pointcloud.ept   Entwine Point Tiles of datasets built before COPC: a directory, legacy
     *   model.nexus      Nexus multiresolution mesh of a Model
     *   model.3dtiles    OGC 3D Tiles of a Model or a .3tz: a directory, its index tileset.json
     *   gsplat.lod       level-of-detail Gaussian Splat (Spark RAD, streamed with range requests)
     *   gsplat.spz       Gaussian Splat in SPZ
     *   gsplat.bounds    local bounding box of a splat (JSON)
     *   gsplat.georef    georeference of a splat (JSON)
     *
     * An artifact: {"role", "path", "kind", "mediaType", "downloadName"?, "index"?, "legacy"?}
     *   path          relative to the build root ("root" below), '/'-separated; URLs append it
     *                 to ".../build/{hash}/" and DDBResolveBuildFile takes it
     *   kind          "file", or "dir" for a directory (index names its descriptor file; its
     *                 other files, e.g. tiles, are "<path>/<file>")
     *   mediaType     of the file a client opens: the file, or the index of a directory
     *   downloadName  the file name to offer for a download; absent when the artifact is not one
     *   index         the descriptor file of a directory, relative to path
     *   legacy        true for an output of an older layout: read when present, never written;
     *                 listed only when the entry has none of its current artifacts
     *
     * The build object of an entry (DDBGetBuildArtifacts, DDBListEntriesEx):
     *   {"buildable": bool, "source": string|null, "hash": string|null, "root": string|null,
     *    "state": "none"|"pending"|"building"|"complete", "missingDependencies": [string],
     *    "lastAttempt": integer|null, "artifacts": [artifact]}
     *   buildable  the entry is built, by itself or as a sidecar of its main file (a .dbf of a .shp)
     *   source     dataset-relative path of the entry the artifacts belong to: the entry itself,
     *              or the main file of a sidecar
     *   hash       the hash of source, which names its build (URLs: ".../build/{hash}/{path}")
     *   root       the absolute build root of source (UTF-8); it may not exist yet
     *   state      in this order: "building" a live build holds the build lock of the entry;
     *              "pending" a build waits for missing dependencies; "complete" the artifacts
     *              are complete (as DDBIsBuildComplete); "none" not built, failed or incomplete
     *   missingDependencies, lastAttempt
     *              from the pending marker when there is one (a pending or a building entry),
     *              [] and null otherwise: dataset-relative names, and the time of the last
     *              attempt in Unix seconds (0 when the marker's time is unreadable)
     *   artifacts  the artifacts that exist (a directory when its index exists), in the order
     *              of the entry type, its primary output first: the file thumbnails and tiles
     *              are made from (raster.cog, pointcloud.copc or pointcloud.ept, vector.gpkg,
     *              model.nexus, gsplat.lod, model.3dtiles of a .3tz)
     *   source, hash and root are null, state "none", the arrays empty for an entry that is not
     *   buildable.
     * Reading build objects changes nothing on disk: a stale lock left by a crashed build is not
     * a build and stays in place (DDBIsBuildActive removes it). A call opens the index once; an
     * entry that is not buildable costs no filesystem access. */

    /** The entry types of libddb and what it does with them (XI-04, ABI 1.1).
     * @param output pointer to C-string where to store the JSON array, by id (free with DDBFree).
     *               Each element: {"id": int, "name": string, "buildable": bool,
     *               "thumbnail": "none"|"file"|"build", "sidecarExtensions": [string],
     *               "roles": [string]}
     *                 id                 the entry type value (entries' "type")
     *                 name               its name, as DDBRescan takes it (case-insensitive)
     *                 thumbnail          "file": rendered from the entry's file; "build": from
     *                                    one of its artifacts, once built; "none": no thumbnail
     *                 sidecarExtensions  lowercase extensions, without the dot, of the files that
     *                                    belong to a main file of this type (same folder and
     *                                    stem; the main file's extension in any letter case): a
     *                                    sidecar's build is the main file's
     *                 roles              every artifact role a build of this type can produce,
     *                                    legacy ones included
     * @return DDBERR_NONE on success, DDBERR_INVALIDARGS when output is NULL */
    DDB_DLL DDBErr DDBGetEntryTypes(char **output);

    /** The build state and the existing artifacts of several entries, with one index open
     * (XI-04, ABI 1.1). For a host it replaces DDBIsBuildable, DDBIsBuildActive,
     * DDBIsBuildComplete, DDBIsBuildPending, DDBGetPendingBuildInfo and DDBGetBuildStatus.
     * @param ddbPath path to a DroneDB database (parent of ".ddb")
     * @param paths array of dataset-relative entry paths (exact match)
     * @param numPaths number of paths (0 is allowed)
     * @param output pointer to C-string where to store the JSON array, one element per input
     *               path, in input order (free with DDBFree):
     *               {"path", "exists": false} for a path that is not indexed, otherwise
     *               {"path", "exists": true, "hash": string|null (null for a folder),
     *                "type": int, "build": <the build object above>}
     * @return DDBERR_NONE on success; DDBERR_INVALIDARGS when ddbPath is empty, paths is NULL
     *         or holds a NULL, numPaths is negative or output is NULL; DDBERR_BUSY on database
     *         contention; DDBERR_EXCEPTION otherwise (no database at ddbPath, ...) */
    DDB_DLL DDBErr DDBGetBuildArtifacts(const char *ddbPath, const char **paths, int numPaths, char **output);

    /** The absolute path of a file of a build, checked (XI-04, ABI 1.1): it serves the
     * ".../build/{hash}/{path}" URLs, tiles included.
     * @param ddbPath path to a DroneDB database (parent of ".ddb")
     * @param hash the hash of the build ("hash" of a build object): 64 hexadecimal characters
     * @param path the file, relative to the build root: an artifact's path, or a file below a
     *             directory artifact ("mvt/3/4/2.pbf"). The dataset path rules apply
     *             (DDBNormalizePath: '/' or '\' separators, no "..", not absolute).
     * @param outPath pointer to C-string where to store the absolute path (UTF-8, free with
     *                DDBFree)
     * @return DDBERR_NONE on success; DDBERR_INVALIDARGS when an argument is NULL or empty, the
     *         hash is malformed or the path breaks the path rules (traversal, absolute, ...);
     *         DDBERR_NOTFOUND when no entry has this hash, the path is not inside one of its
     *         artifacts (lock files and scratch folders are not served), the file does not
     *         exist, or a link leads it out of the build root; DDBERR_BUSY on database
     *         contention */
    DDB_DLL DDBErr DDBResolveBuildFile(const char *ddbPath, const char *hash, const char *path, char **outPath);

    /** Copy the build of an entry to another dataset, with its pending marker (XI-04, ABI 1.1):
     * after copying or moving a file between datasets, its artifacts need no rebuild and a
     * deferred build stays deferred. Copies the artifact folders of the types of the source
     * entries with this hash (legacy ones included), each through a scratch folder renamed onto
     * the destination's, which it replaces; the destination's other files are left alone. Holds
     * the destination's build locks while it copies. The destination needs no entry with this
     * hash yet.
     * @param sourceDdbPath path to the DroneDB database the build is copied from
     * @param destinationDdbPath path to the DroneDB database it is copied to (another dataset)
     * @param hash the hash of the build ("hash" of a build object)
     * @param output pointer to C-string where to store the JSON (free with DDBFree):
     *               {"hash": string, "artifacts": [artifact], "pending": bool}: the artifacts
     *               of the destination after the copy (as in a build object) and whether it has
     *               a pending marker
     * @return DDBERR_NONE on success, also when the source has nothing built (nothing copied);
     *         DDBERR_INVALIDARGS when an argument is NULL or empty, the hash is malformed or
     *         both paths are the same dataset; DDBERR_NOTFOUND when no buildable entry of the
     *         source has this hash; DDBERR_BUILDINPROGRESS when a build of it runs in either
     *         dataset (nothing copied: the destination can build it itself);
     *         DDBERR_RETRYABLE when the destination's disk is (nearly) full
     *         (DDBGetLastErrorReason); DDBERR_BUSY on database contention */
    DDB_DLL DDBErr DDBCopyBuildArtifacts(const char *sourceDdbPath, const char *destinationDdbPath, const char *hash,
                                         char **output);

    /** Options for DDBListEntriesEx (ABI 1.1). Set structSize to sizeof(DDBListEntriesOptions)
     * and every other field explicitly. A later ABI may append fields: a library that does not
     * know them rejects a struct whose unknown bytes are not zero (DDBERR_INVALIDARGS). */
    typedef struct {
        uint32_t structSize; // sizeof(DDBListEntriesOptions)
        bool recursive;      // the whole subtree of the folder (true) or its children only (false)
        bool withDetails;    // full entries (true) or path, hash, type, mtime, size, depth (false)
        bool withBuild;      // each entry gets its "build" object (see the build manifest above)
    } DDBListEntriesOptions;

    DDB_STATIC_ASSERT(sizeof(DDBListEntriesOptions) == 8, "DDBListEntriesOptions layout");
    DDB_STATIC_ASSERT(offsetof(DDBListEntriesOptions, structSize) == 0, "DDBListEntriesOptions layout");
    DDB_STATIC_ASSERT(offsetof(DDBListEntriesOptions, recursive) == 4, "DDBListEntriesOptions layout");
    DDB_STATIC_ASSERT(offsetof(DDBListEntriesOptions, withDetails) == 5, "DDBListEntriesOptions layout");
    DDB_STATIC_ASSERT(offsetof(DDBListEntriesOptions, withBuild) == 6, "DDBListEntriesOptions layout");

    /** DDBListEntries with options (ABI 1.1): with withBuild false the output is DDBListEntries',
     * byte for byte; with withBuild true every entry also has "build", its build object (see
     * the build manifest above), read with the same index connection, so a listing with build
     * states is one call.
     * @param ddbPath path to a DroneDB database (parent of ".ddb")
     * @param folder dataset-relative folder, "" or NULL for the dataset root; a file selects itself
     * @param namePattern file name pattern as DDBListEntries takes it, "" or NULL for every name
     * @param types comma-separated entry types as DDBListEntries takes them, "" or NULL for all
     * @param options see DDBListEntriesOptions
     * @param output pointer to C-string where to store the JSON array, sorted by path (free
     *               with DDBFree)
     * @return DDBERR_NONE on success; DDBERR_INVALIDARGS when ddbPath is empty, options is NULL
     *         or has a structSize below 8 or unknown non-zero fields, types holds an unknown type
     *         or output is NULL; DDBERR_BUSY on database contention */
    DDB_DLL DDBErr DDBListEntriesEx(const char *ddbPath, const char *folder, const char *namePattern,
                                    const char *types, const DDBListEntriesOptions *options, char **output);

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

    /** Generate STAC
     *  @param ddbPath path to the source DroneDB database (parent of ".ddb")
     *  @param entry path of entry in DroneDB database to generate a STAC item for (can be empty)
     *  @param stacCollectionRoot Absolute URL of the STAC collection
     *  @param id STAC asset ID to use instead of folder name (must be unique for entire catalog)
     *  @param stacCatalogRoot Absolute URL of the STAC catalog
     *  @param output pointer to C-string where to store result (JSON) */
    DDB_DLL DDBErr DDBStac(const char *ddbPath, const char *entry, const char *stacCollectionRoot, const char *id, const char *stacCatalogRoot, char **output);

    /** Generate a STAC API ItemCollection (GeoJSON FeatureCollection of STAC Items)
     *  @param ddbPath path to the source DroneDB database (parent of ".ddb")
     *  @param stacCollectionRoot Absolute URL of the STAC collection
     *  @param id STAC collection ID to use instead of folder name (must be unique for entire catalog)
     *  @param stacCatalogRoot Absolute URL of the STAC catalog
     *  @param bbox optional bounding box filter "minX,minY,maxX,maxY" (EPSG:4326), can be empty
     *  @param datetime optional RFC 3339 datetime or interval "start/end" (".." for open ends), can be empty
     *  @param limit maximum number of items to return (<= 0 defaults to 10)
     *  @param offset number of items to skip (paging)
     *  @param output pointer to C-string where to store result (JSON) */
    DDB_DLL DDBErr DDBStacItemCollection(const char *ddbPath, const char *stacCollectionRoot, const char *id, const char *stacCatalogRoot, const char *bbox, const char *datetime, int limit, int offset, char **output);

    /** Data-only STAC (XI-09, ABI 1.1): the document of DDBStac with id NULL, without any member
     *  built from a URL or an ID, which the caller (Registry) owns: no "id", "collection" or
     *  "links", and no asset "href". Everything else is the same: an Item keeps its entry path
     *  as properties.title and as the key of its data asset, a Collection takes its title from
     *  the dataset name or else the dataset folder name.
     *  @param ddbPath path to the source DroneDB database (parent of ".ddb")
     *  @param entry path of the entry to generate a STAC Item for; NULL or "" for the Collection
     *  @param output pointer to C-string where to store result (JSON, free with DDBFree)
     *  @return DDBERR_NONE on success, DDBERR_NOTFOUND if no entry has this path,
     *          DDBERR_INVALIDARGS when ddbPath or output is NULL, an error otherwise */
    DDB_DLL DDBErr DDBStacData(const char* ddbPath, const char* entry, char** output);

    /** Data-only STAC ItemCollection (XI-09, ABI 1.1): the document of DDBStacItemCollection
     *  without "links", and with data-only features (DDBStacData). numberMatched and
     *  numberReturned are kept. bbox, datetime, limit and offset are those of
     *  DDBStacItemCollection.
     *  @param output pointer to C-string where to store result (JSON, free with DDBFree)
     *  @return DDBERR_NONE on success, DDBERR_INVALIDARGS when ddbPath or output is NULL or bbox
     *          is not 4 numbers, an error otherwise */
    DDB_DLL DDBErr DDBStacItemCollectionData(const char* ddbPath,
                                             const char* bbox,
                                             const char* datetime,
                                             int limit,
                                             int offset,
                                             char** output);
    /** Get raster info including bands, detected sensor, presets
     * @param path Path to raster file
     * @param output Pointer to receive JSON string (caller must free with DDBFree)
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBGetRasterInfo(const char *path, char **output);

    /** Get raster statistics and histogram for a band or formula
     * @param path Path to raster file
     * @param formula Optional formula (e.g., "NDVI"), NULL for raw bands
     * @param bandFilter Band order (e.g., "RGB", "RGBNRe"), NULL for auto-detect
     * @param output Pointer to receive JSON string (caller must free with DDBFree)
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBGetRasterMetadata(const char *path, const char *formula, const char *bandFilter, char **output);

    /** Generate memory thumbnail with band mapping, formula, and stretch
     * @param filePath Path to raster file
     * @param size Thumbnail size in pixels
     * @param preset Preset ID (e.g., "true-color"), or NULL
     * @param bands Comma-separated band indices (e.g., "4,3,2"), or NULL
     * @param formula Formula ID (e.g., "NDVI"), or NULL
     * @param bandFilter Band order (e.g., "RGB"), or NULL for auto-detect
     * @param colormap Colormap ID (e.g., "rdylgn"), or NULL for default
     * @param rescale Rescale range "min,max" (e.g., "-1,1"), or NULL for auto
     * @param outBuffer Pointer to output buffer (caller frees with DDBVSIFree)
     * @param outBufferSize Pointer to receive buffer size
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBGenerateMemoryThumbnailEx(const char *filePath, int size,
                                                 const char *preset, const char *bands,
                                                 const char *formula, const char *bandFilter,
                                                 const char *colormap, const char *rescale,
                                                 uint8_t **outBuffer, int *outBufferSize);

    /** Generate memory tile with band mapping, formula, and stretch
     * @param inputPath Path to raster file
     * @param tz Zoom level
     * @param tx X coordinate
     * @param ty Y coordinate
     * @param tileSize Tile size in pixels
     * @param tms TMS numbering
     * @param forceRecreate Force regeneration
     * @param inputPathHash Hash for caching
     * @param preset Preset ID, or NULL
     * @param bands Comma-separated band indices, or NULL
     * @param formula Formula ID, or NULL
     * @param bandFilter Band order, or NULL
     * @param colormap Colormap ID, or NULL
     * @param rescale Rescale range, or NULL
     * @param outBuffer Pointer to output buffer (caller frees with DDBVSIFree)
     * @param outBufferSize Pointer to receive buffer size
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBMemoryTileEx(const char *inputPath,
                                    int tz, int tx, int ty,
                                    int tileSize, bool tms,
                                    bool forceRecreate, const char *inputPathHash,
                                    const char *preset, const char *bands,
                                    const char *formula, const char *bandFilter,
                                    const char *colormap, const char *rescale,
                                    uint8_t **outBuffer, int *outBufferSize);

    /** Validate merge-multispectral inputs
     * @param paths Array of input file paths
     * @param numPaths Number of input file paths
     * @param output Pointer to receive JSON validation result (caller frees with DDBFree)
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBValidateMergeMultispectral(const char **paths, int numPaths, char **output);

    /** Preview merge-multispectral result
     * @param paths Array of input file paths
     * @param numPaths Number of input file paths
     * @param previewBands Comma-separated band indices for RGB preview (e.g., "3,2,1")
     * @param thumbSize Preview size in pixels
     * @param outBuffer Pointer to output buffer (caller frees with DDBVSIFree)
     * @param outBufferSize Pointer to receive buffer size
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBPreviewMergeMultispectral(const char **paths, int numPaths,
                                                 const char *previewBands, int thumbSize,
                                                 uint8_t **outBuffer, int *outBufferSize);

    /** Merge single-band rasters into multi-band GeoTIFF
     * @param paths Array of input file paths
     * @param numPaths Number of input file paths
     * @param outputPath Output GeoTIFF file path
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBMergeMultispectral(const char **paths, int numPaths, const char *outputPath);

    /** Validate that source and reference rasters are compatible for alignment.
     * @param sourcePath Absolute path to source GeoTIFF
     * @param referencePath Absolute path to reference GeoTIFF
     * @param output Pointer to C-string to receive JSON (caller frees with DDBFree)
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBValidateAlignRaster(const char *sourcePath,
                                          const char *referencePath,
                                          char **output);

    /** Align source GeoTIFF to reference GeoTIFF and write output COG.
     * @param sourcePath Absolute path to source GeoTIFF
     * @param referencePath Absolute path to reference GeoTIFF
     * @param outputPath Absolute path for the aligned output (COG). Note: the
     *   output is always reprojected to EPSG:3857 regardless of the source CRS.
     * @param mode "similarity" (default) or "translation"
     * @param output Pointer to C-string to receive JSON AlignResult (caller frees with DDBFree)
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBAlignRaster(const char *sourcePath,
                                  const char *referencePath,
                                  const char *outputPath,
                                  const char *mode,
                                  char **output);

    /** Export raster with visualization params applied as GeoTIFF
     * @param inputPath Path to source raster file
     * @param outputPath Path for the output GeoTIFF file
     * @param preset Preset ID, or NULL
     * @param bands Comma-separated band indices, or NULL
     * @param formula Formula ID (e.g., "NDVI"), or NULL
     * @param bandFilter Band order (e.g., "RGB"), or NULL
     * @param colormap Colormap ID (e.g., "rdylgn"), or NULL
     * @param rescale Rescale range "min,max", or NULL
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBExportRaster(const char *inputPath, const char *outputPath,
                                    const char *preset, const char *bands,
                                    const char *formula, const char *bandFilter,
                                    const char *colormap, const char *rescale);

    /** Export raster with visualization params applied as GeoTIFF, block-windowed.
     * Unlike DDBExportRaster, the formula branch processes the raster in tiles so
     * peak memory is bounded by O(tileSize^2 * bandCount) instead of O(width*height).
     * Supports incremental progress reporting and cooperative cancellation.
     * @param inputPath Path to source raster file
     * @param outputPath Path for the output GeoTIFF file
     * @param preset Preset ID, or NULL
     * @param bands Comma-separated band indices, or NULL
     * @param formula Formula ID (e.g., "NDVI"), or NULL
     * @param bandFilter Band order (e.g., "RGB"), or NULL
     * @param colormap Colormap ID (e.g., "rdylgn"), or NULL
     * @param rescale Rescale range "min,max", or NULL
     * @param tileSize Tile size in pixels for windowed processing; 0 = auto
     * @param progress Optional progress callback (may be NULL)
     * @param progressUserData Opaque pointer forwarded to the progress callback
     * @return DDBERR_NONE on success, DDBERR_CANCELED if canceled, an error otherwise */
    DDB_DLL DDBErr DDBExportRaster2(const char *inputPath, const char *outputPath,
                                    const char *preset, const char *bands,
                                    const char *formula, const char *bandFilter,
                                    const char *colormap, const char *rescale,
                                    int tileSize,
                                    DDBProgressCallback progress, void *progressUserData);

    /** Get info about a single-band raster used for analysis (thermal image, DEM,
     * or generic value raster), including min/max, unit, dimensions and
     * optional thermal calibration.
     * @param filePath Path to raster (R-JPEG thermal or GeoTIFF)
     * @param output Pointer to receive JSON string (caller must free with DDBFree)
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBGetRasterValueInfo(const char *filePath, char **output);

    /** Get raster value at a specific pixel (temperature for thermal, elevation for DEM, etc.).
     * @param filePath Path to raster
     * @param x Pixel X coordinate
     * @param y Pixel Y coordinate
     * @param output Pointer to receive JSON string (caller must free with DDBFree)
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBGetRasterPointValue(const char *filePath, int x, int y, char **output);

    /** Get value statistics (min/max/mean/stddev/median) for a rectangular area.
     * @param filePath Path to raster
     * @param x0 Left X coordinate
     * @param y0 Top Y coordinate
     * @param x1 Right X coordinate
     * @param y1 Bottom Y coordinate
     * @param output Pointer to receive JSON string (caller must free with DDBFree)
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBGetRasterAreaStats(const char *filePath,
                                         int x0, int y0, int x1, int y1,
                                         char **output);

    /** Sample a single-band raster along a GeoJSON LineString (WGS84) and
     * return a JSON profile with equispaced samples (distance in meters,
     * value, lon, lat). Supports DEM/DSM/DTM, thermal rasters and any single
     * band raster with geotransform.
     * @param filePath Path to the raster
     * @param geoJsonLineString GeoJSON geometry (type LineString) in WGS84
     * @param samples Requested number of samples (clamped to [2, 4096];
     *                values <2 fall back to a sensible default)
     * @param output Pointer to receive JSON string (caller must free with DDBFree)
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBGetRasterProfile(const char *filePath,
                                       const char *geoJsonLineString,
                                       int samples,
                                       char **output);

    /** Calculate stockpile volume from a DEM/DSM/DTM raster over a polygon.
     * @param rasterPath Path to single-band elevation raster
     * @param polygonGeoJSON GeoJSON Polygon or MultiPolygon in WGS84
     * @param baseMethod One of: "lowest_perimeter" (default), "average_perimeter",
     *                  "best_fit", "flat"
     * @param flatElevation Elevation used when baseMethod == "flat"
     * @param output Pointer to receive JSON string (caller must free with DDBFree)
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBCalculateVolume(const char *rasterPath,
                                      const char *polygonGeoJSON,
                                      const char *baseMethod,
                                      double flatElevation,
                                      char **output);

    /** Auto-detect a stockpile footprint from a click on the raster.
     * @param rasterPath Path to single-band elevation raster
     * @param lat Click latitude (WGS84)
     * @param lon Click longitude (WGS84)
     * @param radius Search radius in meters (>0)
     * @param sensitivity Detail level in [0, 1]
     * @param output Pointer to receive JSON string (caller must free with DDBFree)
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBDetectStockpile(const char *rasterPath,
                                      double lat, double lon,
                                      double radius,
                                      float sensitivity,
                                      char **output);

    /** Auto-detect ALL stockpile footprints by full-DEM scan.
     * @param rasterPath Path to single-band elevation raster
     * @param sensitivity Detail level in [0, 1]
     * @param minAreaM2 Minimum component area in square meters (>=0)
     * @param maxResults Maximum number of stockpiles returned (>0, capped to 500)
     * @param output Pointer to receive JSON string (caller must free with DDBFree)
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBDetectAllStockpiles(const char *rasterPath,
                                          float sensitivity,
                                          double minAreaM2,
                                          int maxResults,
                                          char **output);

    /** Generate contour lines from a single-band raster (DEM/DSM/DTM).
     *
     * Either `interval` or `count` must be provided. When `interval > 0`
     * the contour spacing is fixed. When `interval <= 0` and `count > 0`
     * the spacing is derived from band statistics: (max - min) / count.
     *
     * NaN sentinel values disable optional clipping bounds.
     *
     * @param rasterPath Path to single-band elevation raster
     * @param interval Vertical spacing between contour levels (<= 0 to use count)
     * @param count Target number of contour levels (<= 0 when interval is used)
     * @param baseOffset Reference base elevation for level alignment
     * @param minElev Drop contours below this elevation (NaN to disable)
     * @param maxElev Drop contours above this elevation (NaN to disable)
     * @param simplifyTolerance Geometry simplification in raster CRS units (0 = none)
     * @param bandIndex 1-based raster band index (1 if unsure)
     * @param output Pointer to receive GeoJSON FeatureCollection (caller frees with DDBFree)
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBGenerateContours(const char *rasterPath,
                                       double interval,
                                       int count,
                                       double baseOffset,
                                       double minElev,
                                       double maxElev,
                                       double simplifyTolerance,
                                       int bandIndex,
                                       char **output);

    /** Mask orthophoto borders making them transparent.
     * Supports 1-band rasters (greyscale ortho, thermal, DEM), 3-band 8-bit RGB and
     * 4-band 8-bit RGBA orthophotos. Transparency is written as an internal GeoTIFF
     * dataset mask (any pre-existing alpha band in RGBA inputs is overwritten).
     * @param input Path to input GeoTIFF
     * @param output Path to output GeoTIFF (with internal dataset mask)
     * @param near Tolerance in grey levels (default 15)
     * @param white If true, search for white borders instead of black
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBMaskBorders(const char *input,
                                   const char *output,
                                   int nearDist,
                                   bool white);

    /** Render a region of a georeferenced raster to a compressed image buffer.
     * Output is produced by GDALWarp with optional resampling and reprojection.
     * Useful for OGC WMS GetMap and OGC API Coverages.
     *
     * @param inputPath Path to source raster (preferably a COG for fast warp).
     * @param bbox Pointer to 4 doubles [minX, minY, maxX, maxY] in @p bboxSrs.
     * @param bboxSrs CRS authority code, e.g. "EPSG:3857" or "EPSG:4326".
     *                If NULL, defaults to "EPSG:4326".
     * @param width Output width in pixels (1..4096).
     * @param height Output height in pixels (1..4096).
     * @param format MIME type: "image/png" | "image/jpeg" | "image/webp" |
     *               "image/tiff" | "image/geotiff".
     *               If NULL, defaults to "image/png".
     * @param outBuffer Receives a newly allocated buffer (free with DDBVSIFree).
     * @param outBufferSize Receives the buffer size in bytes.
     * @return DDBERR_NONE on success, an error otherwise. */
    DDB_DLL DDBErr DDBRenderRasterRegion(const char *inputPath,
                                         const double *bbox,
                                         const char *bboxSrs,
                                         int width, int height,
                                         const char *format,
                                         uint8_t **outBuffer,
                                         int *outBufferSize);

    /** Extended variant of @ref DDBRenderRasterRegion adding optional output CRS
     * and 1-based band selection. Used by OGC WCS GetCoverage with
     * RangeSubset / OutputCRS, and by any client that needs to preserve
     * scientific multi-band data without forced reprojection.
     *
     * @param outputCrs Optional target CRS authority code (e.g. "EPSG:32634").
     *                  NULL or empty = use @p bboxSrs (legacy behaviour).
     * @param bands     Optional pointer to @p bandCount 1-based band indices.
     *                  NULL or @p bandCount = 0 means "all bands".
     * @param bandCount Number of entries in @p bands. */
    DDB_DLL DDBErr DDBRenderRasterRegionEx(const char *inputPath,
                                           const double *bbox,
                                           const char *bboxSrs,
                                           const char *outputCrs,
                                           const int *bands,
                                           int bandCount,
                                           int width, int height,
                                           const char *format,
                                           uint8_t **outBuffer,
                                           int *outBufferSize);

    /** Render a spectral index over a raster region (WMS GetMap STYLES support).
     *
     * Computes NDVI / NDRE / NDWI / EVI / SAVI using the standard 5-band mapping
     * (R=1, G=2, B=3, RedEdge=4, NIR=5) and applies a red-yellow-green color ramp.
     *
     * @param inputPath Source raster path.
     * @param indexName One of: "NDVI", "NDRE", "NDWI", "EVI", "SAVI" (case-insensitive).
     * @param bbox 4 doubles [minX,minY,maxX,maxY] in @p bboxSrs.
     * @param bboxSrs CRS authority code; NULL = "EPSG:4326".
     * @param width Output width in pixels (1..4096).
     * @param height Output height in pixels (1..4096).
     * @param format MIME: "image/png" | "image/jpeg" | "image/webp" | "image/tiff" | "image/geotiff".
     * @param outBuffer Allocated buffer (free with DDBVSIFree).
     * @param outBufferSize Buffer size in bytes.
     * @return DDBERR_NONE on success, an error otherwise. */
    DDB_DLL DDBErr DDBRenderRasterIndex(const char *inputPath,
                                        const char *indexName,
                                        const double *bbox,
                                        const char *bboxSrs,
                                        int width, int height,
                                        const char *format,
                                        uint8_t **outBuffer,
                                        int *outBufferSize);

    /** Query a raster at a geographic point (WMS GetFeatureInfo).
     * Translates (x, y) in @p srs to pixel coords and samples every band.
     *
     * @param inputPath Path to source raster.
     * @param x X coordinate in @p srs.
     * @param y Y coordinate in @p srs.
     * @param srs CRS authority code (e.g. "EPSG:4326"); NULL defaults to EPSG:4326.
     * @param output JSON `{ "bands": [...], "lon": ..., "lat": ..., "pixel": [px,py] }`
     *               (caller frees with DDBFree).
     * @return DDBERR_NONE on success, an error otherwise. */
    DDB_DLL DDBErr DDBQueryRasterPoint(const char *inputPath,
                                       double x, double y,
                                       const char *srs,
                                       char **output);

    /** Query features from a vector dataset (WFS GetFeature / OGC API Items).
     *
     * @param vectorPath Path to the vector source (typically build/{hash}/vec/source.gpkg).
     * @param layerName Layer to query, or NULL to use the first layer.
     * @param bbox Pointer to 4 doubles [minX,minY,maxX,maxY] in @p bboxSrs, or NULL.
     * @param bboxSrs CRS of @p bbox (e.g. "EPSG:4326"); NULL when bbox is NULL.
     * @param maxFeatures Max features to return (clamped to [1, 10000]).
     * @param startIndex 0-based feature offset for pagination.
     * @param outputFormat "application/json" (RFC7946 GeoJSON) or "application/gml+xml".
     *                    NULL defaults to "application/json".
     * @param output Receives the encoded features (caller frees with DDBFree).
     * @return DDBERR_NONE on success, an error otherwise. */
    DDB_DLL DDBErr DDBQueryVector(const char *vectorPath,
                                  const char *layerName,
                                  const double *bbox,
                                  const char *bboxSrs,
                                  int maxFeatures,
                                  int startIndex,
                                  const char *outputFormat,
                                  char **output);

    /** DDBQueryVector restricted to the features with the given ids (WFS RESOURCEID,
     * GetFeatureById, OGC API /items/{id}). Feature ids are the source FIDs: GML ids are
     * "<layer>.<fid>" and GeoJSON features carry "id": fid, the same in every page.
     *
     * @param fids Feature ids to return, or NULL for every feature; a GeoPackage reads
     *             them by primary key.
     * @param numFids Number of ids in @p fids.
     * @param where Attribute filter, or NULL: an SQL expression over the layer's fields
     *              (SQLite syntax for a GeoPackage, e.g. "\"name\" = 'A' AND \"pop\" > '10'").
     *              It is applied as given: build it from quoted identifiers and literals only,
     *              and check the field names first (SQLite reads an unknown double-quoted
     *              identifier as a string literal).
     * Other parameters and the result: see DDBQueryVector. */
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
                                    char **output);

    /** Count the features of a vector layer that match a bbox, feature ids and an attribute
     * filter (WFS numberMatched, OGC API numberMatched), without reading them when the format
     * has a spatial index (GeoPackage) and there is no attribute filter.
     *
     * @param vectorPath, layerName, bbox, bboxSrs, fids, numFids, where See DDBQueryVectorEx.
     * @param count Receives the number of matching features.
     * @return DDBERR_NONE on success, an error otherwise. */
    DDB_DLL DDBErr DDBCountVector(const char *vectorPath,
                                  const char *layerName,
                                  const double *bbox,
                                  const char *bboxSrs,
                                  const long long *fids,
                                  int numFids,
                                  const char *where,
                                  long long *count);

    /** Describe a vector dataset (WFS DescribeFeatureType / OGC API collection).
     *
     * @param vectorPath Path to vector source.
     * @param layerName Layer to describe; NULL = all layers.
     * @param output JSON describing driver, layers, fields, geometryType, srs,
     *               extent and feature count (caller frees with DDBFree).
     * @return DDBERR_NONE on success, an error otherwise. */
    DDB_DLL DDBErr DDBDescribeVector(const char *vectorPath,
                                     const char *layerName,
                                     char **output);

    /** Content of a .prj that gives a CAD drawing the coordinate system @p crs (DDB-12).
     *
     * @param crs An authority code ("EPSG:32632"), WKT or PROJ string, written as ESRI WKT;
     *            or "local" for LOCAL_CS["Drawing"] (the drawing stays in its own coordinates).
     * @param output Receives the .prj content (caller frees with DDBFree).
     * @return DDBERR_NONE on success, DDBERR_EXCEPTION when @p crs is not a coordinate system. */
    DDB_DLL DDBErr DDBMakeCadPrj(const char *crs, char **output);

#ifdef __cplusplus
}
#endif

#endif // DDB_H
