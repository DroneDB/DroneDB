/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef BUILDLOD_RUNNER_H
#define BUILDLOD_RUNNER_H

#include <string>

#include "fs.h"
#include "process_runner.h"
#include "tool_runner_result.h"

namespace ddb
{

    namespace buildlod
    {

        // Locates the build-lod executable (Spark's RAD level-of-detail producer) with
        // tools::findTool (see tool_locator.h for the search order); DDB_BUILDLOD_PATH, when
        // non-empty, is authoritative.
        // Returns an empty path when no usable binary is found. The result is cached for the
        // lifetime of the process; pass forceRefresh=true to bypass the cache.
        fs::path findBuildLodBinary(bool forceRefresh = false);

        // True when a build-lod binary is discoverable (convenience wrapper around
        // findBuildLodBinary). build-lod is MANDATORY for Gaussian Splat (RAD) builds:
        // buildGsplat throws AppException (the build fails) when the tool is absent.
        bool isBuildLodAvailable();

        // What to convert and how
        struct BuildLodRequest
        {
            fs::path input{};
            fs::path outputRad{};
            bool quality = true;
            int maxSh = 1;
            fs::path binary{}; // empty: findBuildLodBinary()
        };

        // Runs build-lod to convert a splat source (.ply | .spz | .splat | .ksplat | .sog)
        // into a single Spark RAD LOD file at request.outputRad, using the web delivery profile:
        // bhatt-lod ("quality") with spherical harmonics capped at maxSh, single-file --rad.
        //
        // build-lod writes "<input-stem>-lod.rad" next to the file it is given, and skips a file
        // named "*-lod.spz" or "*-lod.rad". So the input is linked (else copied) as
        // "source.<ext>" into a scratch folder beside outputRad, build-lod runs on that, and the
        // scratch folder is removed on every path: the folder of the input is never written to,
        // and a file there that looks like build-lod's output is never touched. The result
        // succeeds when the tool exited with code 0 and left a non-empty RAD that is moved to
        // outputRad; otherwise it carries the error with the tail of the tool's output, which is
        // where build-lod says why it wrote nothing (it exits 0 after a decode failure). The
        // binary path is resolved via findBuildLodBinary() unless request.binary is non-empty.
        // DDB_BUILDLOD_IDLE_TIMEOUT_SECONDS overrides the supervised idle deadline
        // on every call, including Gaussian Splat entry builds. Set a decimal
        // integer from 1 to 86400 seconds (24 hours); empty/unset preserves Options
        // deadline (60 minutes by default). Invalid/overflowing values return an
        // actionable permanent failure before changing output files or launching.
        ToolRunResult runBuildLodResult(const BuildLodRequest &request,
                                                  const process::Options &options = process::Options(),
                                                  const process::IProcessRunner &runner = process::systemProcessRunner());

    } // namespace buildlod

} // namespace ddb

#endif // BUILDLOD_RUNNER_H
