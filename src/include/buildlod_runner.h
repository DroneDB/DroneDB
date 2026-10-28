/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef BUILDLOD_RUNNER_H
#define BUILDLOD_RUNNER_H

#include <chrono>
#include <cstdint>
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

        // The longest silence allowed to build-lod, and to DDB_BUILDLOD_IDLE_TIMEOUT_SECONDS: it
        // keeps the steady-clock deadline of the process runner representable.
        constexpr std::chrono::hours kMaxIdleTimeout{24};

        // Idle limit per million splats of the input. build-lod prints nothing during its sort,
        // each level of its tree, its pruning and its RAD write, and each of these grows with the
        // number of splats. The longest silence measured was ~134 s for 31.6 M splats on a fast
        // host (~4.2 s per million); a slow host took ~7.5 times as long overall and then stayed
        // silent for more than an hour after its pruning, so the limit keeps a large margin over
        // both (31.6 M splats: ~4.2 hours).
        constexpr std::chrono::minutes kIdleTimeoutPerMillionSplats{8};

        // Bytes per splat of a PLY with SH degree 3 (59 floats: position, opacity, scale,
        // rotation, 48 SH coefficients; 7.45 GB for 31.6 M splats): the splat count of a file
        // whose header gives none is estimated from its size with it.
        constexpr std::uintmax_t kPlyBytesPerSplat = 236;

        // The idle limit for an input of `splats` splats: kIdleTimeoutPerMillionSplats per
        // million, rounded up to a whole second, never under process::kDefaultIdleTimeout (the
        // limit of every other tool) and never over kMaxIdleTimeout.
        std::chrono::milliseconds idleTimeoutForSplats(std::uint64_t splats);

        // The idle limit for a build-lod run on `input`: from the splat count its header gives
        // (getGaussianSplatInfo: PLY, .splat, SPZ, .ksplat), else from its size at
        // kPlyBytesPerSplat (a .sog; undercounts a compressed source, so its floor applies more
        // often), else (it cannot be read) the floor. DDB_BUILDLOD_IDLE_TIMEOUT_SECONDS still
        // overrides what is passed to runBuildLodResult. Never throws.
        std::chrono::milliseconds idleTimeoutFor(const fs::path &input);

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
        // "source.<ext>" into a scratch folder "<folder of outputRad>-build-lod-<rand>" beside the
        // folder of outputRad (io::ScopedTempDir::createBeside), build-lod runs on that, and the
        // scratch folder is removed on every path: the folder of the input is never written to, a
        // file there that looks like build-lod's output is never touched, and the folder of the
        // output holds the RAD only, even when the scratch cannot be removed. A symbolic link is
        // staged as it is: build-lod names its output after the path it is given (it does not
        // resolve links), so it writes in the scratch folder, never beside the dataset file. The
        // extension keeps its case: build-lod matches it case-insensitively. The result
        // succeeds when the tool exited with code 0 and left a non-empty RAD that is moved to
        // outputRad; otherwise it carries the error with the tail of the tool's output, which is
        // where build-lod says why it wrote nothing (it exits 0 after a decode failure). The
        // binary path is resolved via findBuildLodBinary() unless request.binary is non-empty.
        // DDB_BUILDLOD_IDLE_TIMEOUT_SECONDS overrides the supervised idle deadline
        // on every call, including Gaussian Splat entry builds. Set a decimal
        // integer from 1 to 86400 seconds (24 hours); empty/unset preserves Options
        // deadline (60 minutes by default; a Gaussian Splat entry build passes
        // idleTimeoutFor(input)). Invalid/overflowing values return an
        // actionable permanent failure before changing output files or launching.
        ToolRunResult runBuildLodResult(const BuildLodRequest &request,
                                                  const process::Options &options = process::Options(),
                                                  const process::IProcessRunner &runner = process::systemProcessRunner());

    } // namespace buildlod

} // namespace ddb

#endif // BUILDLOD_RUNNER_H
