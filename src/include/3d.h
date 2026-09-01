/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef _3D_H
#define _3D_H

#include <cstdint>
#include <optional>
#include <string>
#include "ddb_export.h"
#include "entry_types.h"
#include "fs.h"
#ifndef NO_NEXUS
#include <nxs.h>
#endif
#include <vector>
#include "obj2tiles_runner.h"

namespace ddb
{

    /**
     * @brief WGS84 georeferencing origin for a 3D model.
     *
     * The model's local coordinate frame (meters, ENU-like) is assumed to be
     * centered at this latitude/longitude/altitude, matching the OpenDroneMap /
     * Obj2Tiles convention. Used to build the ECEF transform in the 3D Tiles
     * `tileset.json` so the model sits in the correct place on the globe.
     */
    struct ModelGeoref
    {
        double latitude = 0.0;  ///< WGS84 latitude of the model origin, degrees.
        double longitude = 0.0; ///< WGS84 longitude of the model origin, degrees.
        double altitude = 0.0;  ///< Altitude of the model origin, meters.
    };

    DDB_DLL std::string buildNexus(const std::string &inputObj, const std::string &outputNxs, bool overwrite = false);

    /**
     * @brief Detect a model's WGS84 georeferencing origin from sidecar files.
     *
     * Looks next to @p inputObj (and one directory up, plus an `opensfm/` sibling
     * for OpenDroneMap project layouts) for one of these JSON sidecars, in order:
     * `<stem>.geo.json`, `georef.json`, `reference_lla.json`. Each is parsed for
     * `latitude`/`longitude` (required) and `altitude` (optional, default 0);
     * the short keys `lat`/`lon`/`alt` are also accepted.
     *
     * @param inputObj Path to the source model (OBJ/GLTF/GLB).
     * @return The georeferencing origin, or std::nullopt if none is found or the
     *         coordinates are out of range (then the model is treated as local).
     */
    DDB_DLL std::optional<ModelGeoref> detectModelGeoref(const std::string &inputObj);

    /**
     * @brief Generate an OGC 3D Tiles tileset from an OBJ/GLTF/GLB model using Obj2Tiles.
     *
     * Mirrors @ref buildNexus: GLTF/GLB inputs are converted to OBJ first and file
     * dependencies are validated. The Obj2Tiles binary is invoked as a subprocess
     * (see obj2tiles_runner.h). On success the output directory contains
     * `tileset.json` and one or more `LOD-<n>` folders with `.b3dm` tiles.
     *
     * The write is atomic: tiles are produced into a sibling temporary directory and
     * renamed onto @p outputDir only after a valid `tileset.json` is produced.
     *
     * Georeferencing: if @p georef is provided it is used; otherwise, when
     * @p autoDetectGeoref is true (the default), @ref detectModelGeoref is used to
     * find a sidecar next to @p inputObj. When neither yields coordinates the model
     * is tiled in local mode (identity transform), preserving Nexus-viewer parity.
     *
     * @param inputObj Path to the input OBJ (or GLTF/GLB) model.
     * @param outputDir Destination directory for the tileset (e.g. `<hash>/3dtiles`).
     * @param overwrite When true, an existing non-empty @p outputDir is replaced; when
     *                  false, a non-empty @p outputDir causes an exception.
     * @param georef Optional explicit georeferencing origin (overrides auto-detection).
     * @param autoDetectGeoref When true and @p georef is empty, attempt sidecar detection.
     * @return The path to the generated `tileset.json`.
     * @throws Obj2TilesException if the Obj2Tiles binary is unavailable or fails.
     * @throws BuildDepMissingException if model dependencies (textures/buffers) are missing.
     */
    DDB_DLL std::string buildModel3DTiles(const std::string &inputObj, const std::string &outputDir,
                                          bool overwrite = false,
                                          std::optional<ModelGeoref> georef = std::nullopt,
                                          bool autoDetectGeoref = true);

    /**
     * @brief Local-space axis-aligned bounding box of a 3D model.
     *
     * Bounds are expressed in the model's own local coordinate frame (meters),
     * following the OpenDroneMap / Obj2Tiles ENU convention where X is East, Y is
     * North and Z is up. Combined with a @ref ModelGeoref origin this yields the
     * model's WGS84 footprint.
     */
    struct ModelInfo
    {
        bool hasBounds = false;                     ///< True when the box below is valid.
        double minX = 0.0, minY = 0.0, minZ = 0.0;  ///< Minimum corner (local meters).
        double maxX = 0.0, maxY = 0.0, maxZ = 0.0;  ///< Maximum corner (local meters).
        uint64_t faceCount = 0;                     ///< Total number of mesh faces (triangles after triangulation).
    };

    /**
     * @brief Read a 3D model's local-space axis-aligned bounding box via Assimp.
     *
     * Supports every mesh format Assimp can import (OBJ/PLY/GLTF/GLB). Node
     * transforms are baked in so the bounds are in the model's root frame.
     * Best-effort: returns false (leaving @p info untouched) when the model cannot
     * be read or has no vertices, so a model can still be indexed without a
     * footprint instead of failing the whole parse.
     *
     * @param inputModel Path to the model file.
     * @param info Output bounding box (only written when the function returns true).
     * @return true when a non-empty bounding box was computed, false otherwise.
     */
    DDB_DLL bool getModelInfo(const std::string &inputModel, ModelInfo &info);

    /**
     * @brief Compute Obj2Tiles parameters based on model face count.
     *
     * Maps info.faceCount to a (divisions, lods, octree) band from the spec.
     * The resulting tile-hierarchy depth (divisions + lods - 1 when octree)
     * is always <= MAX_TILE_DEPTH (6, i.e. <= 4096 finest-LOD tiles),
     * regardless of faceCount, so pathologically large models cannot make
     * Obj2Tiles produce an unbounded number of b3dm files.
     *
     * @param info Model info with faceCount populated by getModelInfo().
     * @return Obj2TilesOptions with heuristic-based divisions/lods/octree.
     */
    DDB_DLL obj2tiles::Obj2TilesOptions computeObj2TilesOpts(const ModelInfo &info);

    DDB_DLL std::vector<std::string> getObjDependencies(const std::string &obj);
    DDB_DLL std::vector<std::string> getGltfDependencies(const std::string &gltf);

    /**
     * @brief Classify a glTF/GLB file from the primitives declared in its JSON.
     *
     * The .gltf/.glb container is not limited to triangle meshes: it is also used
     * to ship point geometry (for instance the 3D Gaussian Splatting tiles emitted
     * by 3D Tiles exporters, which use `mode: 0` plus KHR_gaussian_splatting) and
     * line geometry. Only triangle geometry can produce a 3D model build artifact,
     * so everything else is reported as EntryType::Generic and is consequently not
     * buildable (see isBuildableInternal in build.cpp).
     *
     * Mirrors @ref identifyPly: best-effort and never throws. A missing, unreadable
     * or malformed file is reported as EntryType::Generic, so it can still be
     * indexed instead of failing the whole parse.
     *
     * @param gltfFile Path to the .gltf or .glb file.
     * @return EntryType::Model when triangle geometry is present, EntryType::Generic otherwise.
     */
    DDB_DLL EntryType identifyGltf(const fs::path &gltfFile);

    /**
     * @brief Convert glTF/GLB to OBJ or PLY format
     *
     * @param inputGltf Path to the input glTF or GLB file
     * @param outputBasePath Base path for output files (without extension)
     * @param outGeomPath Output parameter for the generated geometry file path
     * @param outMtlPath Output parameter for the generated MTL file path (OBJ only)
     * @param forcePLY Force PLY format output even if UVs are present
     * @param preferPLYIfNoUV Prefer PLY format if no UVs and vertex colors are present
     * @throws AppException if conversion fails
     */
    DDB_DLL void convertGltfTo3dModel(const std::string &inputGltf,
                                      const std::string &outputBasePath,
                                      std::string &outGeomPath,
                                      std::string &outMtlPath,
                                      bool forcePLY = false,
                                      bool preferPLYIfNoUV = true);

}
#endif // _3D_H
