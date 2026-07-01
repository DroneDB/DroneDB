/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef OBJ2TILES_RUNNER_H
#define OBJ2TILES_RUNNER_H

#include <optional>
#include <string>
#include "ddb_export.h"
#include "fs.h"

namespace ddb
{

    namespace obj2tiles
    {

        // Options forwarded to the Obj2Tiles CLI. Defaults mirror the Obj2Tiles
        // CLI defaults (divisions=2, lods=3). When localMode is true the model is
        // tiled with an identity transform (no ECEF georeferencing) and lat/lon/alt
        // are ignored. The octree and lodTextureScale options are only understood by
        // the enhanced fork (OpenDroneMap/Obj2Tiles#95); their flags are emitted only
        // when set to a non-default value, keeping compatibility with upstream v1.4.0.
        struct Obj2TilesOptions
        {
            int lods = 3;                 // --lods
            int divisions = 2;            // --divisions
            bool localMode = true;        // --local (identity matrix, non-georeferenced)
            std::optional<double> lat;    // --lat  (ignored when localMode)
            std::optional<double> lon;    // --lon  (ignored when localMode)
            double alt = 0.0;             // --alt  (ignored when localMode)
            bool octree = false;          // --octree (fork only)
            double lodTextureScale = 1.0; // --lod-texture-scale (fork only)
        };

        // Locates the Obj2Tiles executable using the following discovery order:
        //   1. DDB_OBJ2TILES_PATH env var (authoritative when non-empty)
        //   2. Folder of the current executable (bundled next to ddbcmd/ddbtest)
        //   3. <exe_folder>/../bin (cmake --install layout)
        //   4. Directories on the system PATH
        // Returns an empty path if no usable binary is found. The result is cached
        // for the lifetime of the process; pass forceRefresh=true to bypass the cache.
        DDB_DLL fs::path findObj2TilesBinary(bool forceRefresh = false);

        // Runs the Obj2Tiles binary to convert a single OBJ into an OGC 3D Tiles
        // tileset (tileset.json + LOD-*/*.b3dm) under outDir. Returns true on success
        // (binary exited with code 0 and outDir/tileset.json exists). On failure
        // returns false and leaves a message in errorOut. The binary path is resolved
        // via findObj2TilesBinary() unless a non-empty path is provided.
        DDB_DLL bool runObj2Tiles(const fs::path &inputObj,
                                  const fs::path &outDir,
                                  const Obj2TilesOptions &opts,
                                  std::string &errorOut,
                                  const fs::path &binary = fs::path());

    } // namespace obj2tiles

} // namespace ddb

#endif // OBJ2TILES_RUNNER_H
