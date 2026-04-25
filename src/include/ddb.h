/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DDB_H
#define DDB_H

#include "ddb_export.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DDB_LOG_ENV "DDB_LOG"
#define DDB_DEBUG_ENV "DDB_DEBUG"

#define DDB_FOLDER ".ddb"

    enum DDBErr
    {
        DDBERR_NONE = 0,      // No error
        DDBERR_EXCEPTION = 1, // Generic app exception
        DDBERR_BUILDDEPMISSING = 2
    };

#define DDB_C_BEGIN \
    try             \
    {
#define DDB_C_END                  \
    }                              \
    catch (const AppException &e)  \
    {                              \
        DDBSetLastError(e.what()); \
        return DDBERR_EXCEPTION;   \
    }                              \
    catch (const std::bad_alloc &e) \
    {                              \
        DDBSetLastError("Out of memory"); \
        return DDBERR_EXCEPTION;   \
    }                              \
    catch (const std::exception &e) \
    {                              \
        DDBSetLastError(e.what()); \
        return DDBERR_EXCEPTION;   \
    }                              \
    catch (...)                    \
    {                              \
        DDBSetLastError("Unknown error occurred"); \
        return DDBERR_EXCEPTION;   \
    }                              \
    return DDBERR_NONE;

    extern char ddbLastError[255];
    void DDBSetLastError(const char *err);

    /** Get the last error message
     * @return last error message */
    DDB_DLL const char *DDBGetLastError();

    /** This must be called as the very first function
     * of every DDB process/program
     * @param verbose whether the program should output log messages to stdout */
    DDB_DLL void DDBRegisterProcess(bool verbose = false);

    /** Get library version */
    DDB_DLL const char *DDBGetVersion();

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
    DDB_DLL DDBErr DDBAdd(const char *ddbPath, const char **paths, int numPaths, char **output, bool recursive = false);

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
    DDB_DLL DDBErr DDBInfo(const char **paths, int numPaths, char **output, const char *format = "text", bool recursive = false, int maxRecursionDepth = 0,
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

    /** Rescan all files in the index to update metadata
     * @param ddbPath path to a DroneDB database (parent of ".ddb")
     * @param output pointer to C-string where to store output (JSON array of results)
     * @param types comma-separated list of entry types to rescan (e.g., "image,geoimage,pointcloud"), or empty for all.
     *              Valid types: generic, geoimage, georaster, pointcloud, image, dronedb, markdown, video, geovideo, model, panorama, geopanorama, vector
     * @param stopOnError whether to stop processing on first error (default true)
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBRescan(const char *ddbPath, char **output, const char *types = "", bool stopOnError = true);

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

    /** IsBuildActive
     * @param ddbPath path to the source DroneDB database (parent of ".ddb")
     * @param path Entry path to check if build is active for this entry
     * @param isBuildActive if a build is currently active for this entry
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBIsBuildActive(const char *ddbPath, const char *path, bool *isBuildActive);

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

    /** Mask orthophoto borders making them transparent
     * @param input Path to input GeoTIFF
     * @param output Path to output GeoTIFF (with alpha band)
     * @param near Tolerance in grey levels (default 15)
     * @param white If true, search for white borders instead of black
     * @return DDBERR_NONE on success, an error otherwise */
    DDB_DLL DDBErr DDBMaskBorders(const char *input,
                                   const char *output,
                                   int nearDist,
                                   bool white);

#ifdef __cplusplus
}
#endif

#endif // DDB_H
