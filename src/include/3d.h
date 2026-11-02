/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef _3D_H
#define _3D_H

#include <cstdint>
#include <optional>
#include <string>
#include "build_budget.h"
#include "entry_types.h"
#include "fs.h"
#include "local_frame_georef.h"
#include <vector>
#include "obj2tiles_runner.h"

namespace ddb
{

    /** What @ref buildModel3DTiles may be told besides the model and the destination. */
    struct Tiles3DOptions
    {
        /// When true, an existing non-empty output directory is replaced (only once the new
        /// tileset is complete); when false, a non-empty one causes an exception.
        bool overwrite = false;
        /// Explicit georeferencing (overrides auto-detection). The tileset is placed by an ENU origin:
        /// the offset point of a projected georef is converted to WGS84 and used as that origin, so
        /// the axes of the model are taken as East/North/Up there, as the grid of the CRS differs from
        /// that by the convergence of the meridians (about 1 degree at the edge of a UTM zone).
        std::optional<LocalFrameGeoref> georef{};
        /// When true and @c georef is empty, attempt sidecar detection.
        bool autoDetectGeoref = true;
        /// Root folder of the dataset that contains the model, or empty when unknown (same
        /// meaning as in @ref buildBm3d).
        std::string datasetRoot{};
        /// What the caller asks of the build: the cap on the cores Obj2Tiles may use (it lowers
        /// DDB_OBJ2TILES_MAX_CORES, never raises it)
        BuildOptions build{};
    };

    /**
     * @brief Build a bm3d (.bm3d) file from an OBJ/GLTF/GLB/PLY/STL model.
     *
     * File dependencies are validated first (see @ref getObjDependencies; for a PLY
     * only its `comment TextureFile`, read from the header, see getPlyTexture()).
     * An STL and an untextured PLY are read by bm3d as they are; a textured PLY is converted to
     * OBJ + MTL + texture (convertPlyToObj()) and a GLTF/GLB to OBJ (or to PLY when
     * it has no texture coordinates) in a temporary folder first.
     * @param outputBm3d Output file; when empty, `<input stem>.bm3d` next to @p inputObj.
     * @param datasetRoot Root folder of the dataset that contains @p inputObj, or empty
     *                    when unknown. When set, OBJ/MTL/PLY texture references that
     *                    resolve outside it are rejected. It never applies to the
     *                    temporary files produced from a GLTF/GLB or PLY input.
     * @param options What the caller asks of the build: the threads bm3d may use.
     * @throws BuildDepMissingException if a dependency (MTL, texture, buffer) is missing
     *         and could still be added to the dataset.
     * @throws AppException if a reference is absolute or escapes @p datasetRoot (it can
     *         never be satisfied), if a GLTF/GLB buffer or image URI is absolute or
     *         leaves the glTF's folder (only files in that folder or below are
     *         staged for the converted model), if the converted model still
     *         references a file that cannot be resolved, or on any other failure.
     */
    std::string buildBm3d(const std::string& inputObj,
                          const std::string& outputBm3d,
                          bool overwrite = false,
                          const std::string& datasetRoot = "",
                          const BuildOptions& options = {});

    /**
     * @brief Generate an OGC 3D Tiles tileset from an OBJ/GLTF/GLB/PLY/STL model using Obj2Tiles.
     *
     * Mirrors @ref buildBm3d: file dependencies are validated, then GLTF/GLB and STL
     * inputs are converted to OBJ (vertex colours kept as `v x y z r g b` when there
     * are no texture coordinates) and PLY inputs are streamed to OBJ (+ MTL + texture) by
     * convertPlyToObj(), whose face count and bounds replace the Assimp pass of
     * @ref getModelInfo. The Obj2Tiles binary is invoked as a subprocess
     * (see obj2tiles_runner.h). On success the output directory contains
     * `tileset.json` and one or more `LOD-<n>` folders with `.b3dm` tiles.
     *
     * Lock protocol: the primary lock of the owning build (`bm3d` for a Model) comes
     * first, then the `3dtiles` lock taken here; cleanupBuild follows the same order
     * when it handles the `-temp-`/`-old` siblings, so a Model build can only meet
     * cleanup on its primary lock (benign, retried by the callers) and never on the
     * tiles lock. A caller that may overlap cleanupBuild must therefore hold the
     * primary lock before calling this function. The standalone `ddb tiles3d`
     * command holds only the tiles lock.
     *
     * Safe rebuild: a @ref BuildLock on @p outputDir (`<outputDir>.building`) is held
     * until return. Under it, a usable `<outputDir>-old` left by an interrupted swap is
     * restored when the final tiles sentinel is missing/empty. The backup is deleted
     * only when the final sentinel is a nonempty regular file. Tiles are then
     * produced into a sibling `<outputDir>-temp-<rand>` (removed on any failure) and
     * @p outputDir is left untouched until Obj2Tiles succeeds; then @p outputDir is
     * renamed to `<outputDir>-old`, the temp folder to @p outputDir, and `-old` is
     * deleted. A failed rebuild therefore keeps the previous tileset. Trailing
     * separators in @p outputDir are ignored.
     *
     * Georeferencing: if @p georef is provided it is used; otherwise, when
     * @p autoDetectGeoref is true (the default), @ref detectLocalFrameGeoref is used to
     * find a sidecar next to @p inputObj. When neither yields coordinates the model
     * is tiled in local mode (identity transform), as the bm3d viewer shows it.
     *
     * @param inputObj Path to the input OBJ (or GLTF/GLB) model.
     * @param outputDir Destination directory for the tileset (e.g. `<hash>/3dtiles`).
     * @param options See @ref Tiles3DOptions.
     * @return The path to the generated `tileset.json`.
     * @throws Obj2TilesException if the Obj2Tiles binary is unavailable or fails.
     * @throws BuildInProgressException if another tiles build holds the lock on @p outputDir.
     * @throws BuildDepMissingException if model dependencies (textures/buffers) are missing.
     * @throws AppException if a dependency reference is unsatisfiable (same cases as
     *         @ref buildBm3d).
     */
    std::string buildModel3DTiles(const std::string &inputObj, const std::string &outputDir,
                                  const Tiles3DOptions &options = {});

    /** Read-only sentinel check used by tiles recovery and build cleanup. */
    bool isModel3DTilesComplete(const fs::path &outputDir);

    /** A usable swap backup exists while the final tiles sentinel is missing/empty. */
    bool hasRecoverableModel3DTilesSwap(const fs::path &outputDir);

    /**
     * Repair an interrupted tiles swap under the output's tiles BuildLock.
     * A full Model build must already hold its primary lock before calling this
     * (lock order: primary `bm3d` lock, then `3dtiles`; cleanupBuild uses the same
     * order, see @ref buildModel3DTiles).
     * Throws BuildInProgressException without changing directories when busy;
     * filesystem failures preserve the usable backup and are propagated.
     */
    void recoverModel3DTilesSwap(const std::string &outputDir);

    /**
     * @brief Local-space axis-aligned bounding box of a 3D model.
     *
     * Bounds are expressed in the model's own local coordinate frame (meters),
     * following the OpenDroneMap / Obj2Tiles ENU convention where X is East, Y is
     * North and Z is up. Combined with a @ref LocalFrameGeoref this yields the
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
     * Supports every mesh format Assimp can import (OBJ/PLY/STL/GLTF/GLB). Node
     * transforms are baked in so the bounds are in the model's root frame.
     * Best-effort: returns false (leaving @p info untouched) when the model cannot
     * be read or has no vertices, so a model can still be indexed without a
     * footprint instead of failing the whole parse.
     *
     * @param inputModel Path to the model file.
     * @param info Output bounding box (only written when the function returns true).
     * @return true when a non-empty bounding box was computed, false otherwise.
     */
    bool getModelInfo(const std::string &inputModel, ModelInfo &info);

    /**
     * @brief Compute Obj2Tiles parameters based on model face count.
     *
     * Maps info.faceCount to a (divisions, lods, octree) band.
     * The resulting tile-hierarchy depth (divisions + lods - 1 when octree)
     * is always <= MAX_TILE_DEPTH (6, i.e. <= 4096 finest-LOD tiles),
     * regardless of faceCount, so pathologically large models cannot make
     * Obj2Tiles produce an unbounded number of b3dm files.
     *
     * @param info Model info with faceCount populated by getModelInfo() or by the
     *             PLY converter (PlyToObjResult::info).
     * @return Obj2TilesOptions with heuristic-based divisions/lods/octree.
     */
    obj2tiles::Obj2TilesOptions computeObj2TilesOpts(const ModelInfo &info);

    /**
     * @brief List the file dependencies of an OBJ (its MTL files and their textures).
     *
     * Path contract: `mtllib` resolves against the OBJ's folder; a texture referenced
     * by an MTL is present if it exists relative to the MTL's folder, otherwise relative
     * to the OBJ's folder. References are normalised (`\` becomes `/`, then
     * lexically normalised) and duplicates are dropped. Names are returned relative to
     * the OBJ's folder: same-folder dependencies are bare names, and a texture of
     * `mats/m.mtl` is returned as `mats/<texture>`.
     *
     * @param obj Path to the OBJ file.
     * @param datasetRoot Root folder of the dataset that contains @p obj, or empty when
     *                    unknown. When set, references resolving outside it are rejected.
     * @throws FSException if @p obj does not exist.
     * @throws AppException if a reference is absolute, or escapes @p datasetRoot.
     */
    // Tests read the dependencies of an OBJ through it to check the path contract above.
    std::vector<std::string> getObjDependencies(const std::string &obj, const std::string &datasetRoot = "");

    /**
     * @brief Classify a glTF/GLB file from the primitives declared in its JSON.
     *
     * The .gltf/.glb container is not limited to triangle meshes: it is also used
     * to ship point geometry (for instance the 3D Gaussian Splatting tiles emitted
     * by 3D Tiles exporters, which use `mode: 0` plus KHR_gaussian_splatting) and
     * line geometry. Only triangle geometry can produce a 3D model build artifact,
     * so everything else is reported as EntryType::Generic and is consequently not
     * buildable (see the Model entry handler, entry_handler_model.cpp).
     *
     * Mirrors @ref identifyPly: best-effort and never throws. A missing, unreadable
     * or malformed file is reported as EntryType::Generic, so it can still be
     * indexed instead of failing the whole parse. A text .gltf too large to parse
     * cheaply on the indexing path falls back to EntryType::Model.
     *
     * @param gltfFile Path to the .gltf or .glb file.
     * @return EntryType::Model when triangle geometry is present, EntryType::Generic otherwise.
     */
    EntryType identifyGltf(const fs::path &gltfFile);

    /**
     * @brief Convert a model Assimp reads (glTF/GLB, STL) to OBJ or PLY format
     *
     * @param inputGltf Path to the input glTF, GLB or STL file
     * @param outputBasePath Base path for output files (without extension)
     * @param outGeomPath Output parameter for the generated geometry file path
     * @param outMtlPath Output parameter for the generated MTL file path (OBJ only)
     * @param forcePLY Force PLY format output even if UVs are present
     * @param preferPLYIfNoUV Write PLY instead of OBJ when the scene has no UVs (the OBJ
     *                        output keeps vertex colours as `v x y z r g b` either way)
     * @throws AppException if conversion fails
     */
    void convertGltfTo3dModel(const std::string &inputGltf,
                              const std::string &outputBasePath,
                              std::string &outGeomPath,
                              std::string &outMtlPath,
                              bool forcePLY = false,
                              bool preferPLYIfNoUV = true);

}
#endif // _3D_H
