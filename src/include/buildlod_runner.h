/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef BUILDLOD_RUNNER_H
#define BUILDLOD_RUNNER_H

#include <string>

#include "ddb_export.h"
#include "fs.h"

namespace ddb
{

    namespace buildlod
    {

        // Locates the build-lod executable (Spark's RAD level-of-detail producer) using the
        // same discovery order as the Untwine accelerator:
        //   1. DDB_BUILDLOD_PATH env var (authoritative when non-empty)
        //   2. Folder of the current executable (bundled next to ddbcmd/ddbtest by packaging)
        //   3. <exe_folder>/../bin (cmake --install layout)
        //   4. Directories on the system PATH
        // Returns an empty path when no usable binary is found. The result is cached for the
        // lifetime of the process; pass forceRefresh=true to bypass the cache.
        DDB_DLL fs::path findBuildLodBinary(bool forceRefresh = false);

        // True when a build-lod binary is discoverable (convenience wrapper around
        // findBuildLodBinary). LOD generation is optional, so callers use this to decide
        // whether to attempt it and degrade gracefully (serve plain model.spz) when absent.
        DDB_DLL bool isBuildLodAvailable();

        // Runs build-lod to convert a splat source (.ply | .spz | .splat | .ksplat | .sog)
        // into a single Spark RAD LOD file at outputRad, using the web delivery profile:
        // bhatt-lod ("quality") with spherical harmonics capped at maxSh, single-file --rad.
        //
        // build-lod always writes "<input-stem>-lod.rad" next to the input file; this
        // function moves that result to outputRad. Returns true on success (the tool exited
        // with code 0 and outputRad exists). On failure returns false, leaves a message in
        // errorOut, and removes any partial output. The binary path is resolved via
        // findBuildLodBinary() unless a non-empty path is provided.
        DDB_DLL bool runBuildLod(const fs::path &input,
                                 const fs::path &outputRad,
                                 std::string &errorOut,
                                 bool quality = true,
                                 int maxSh = 1,
                                 const fs::path &binary = fs::path());

    } // namespace buildlod

} // namespace ddb

#endif // BUILDLOD_RUNNER_H
