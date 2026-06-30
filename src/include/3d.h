/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef _3D_H
#define _3D_H

#include <optional>
#include <string>
#include "ddb_export.h"
#ifndef NO_NEXUS
#include <nxs.h>
#endif
#include <vector>

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

    DDB_DLL std::vector<std::string> getObjDependencies(const std::string &obj);
    DDB_DLL std::vector<std::string> getGltfDependencies(const std::string &gltf);

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
