/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef BM3D_RUNNER_H
#define BM3D_RUNNER_H

#include "build_budget.h"
#include "fs.h"
#include "process_runner.h"

namespace ddb {

// Nested in ddb on purpose: scripts/check-gpl-linkage.py looks for top-level bm3d:: symbols
// (the code of the GPL-3.0 program) in libddb, and this runner must not look like them
namespace bm3d {

// The program that writes bm3d files (the separate GPL-3.0 program of vendor/bm3d), and the
// environment variable that names its file (tools::findTool takes both)
inline constexpr const char* kBm3dName = "bm3d";
inline constexpr const char* kBm3dEnv = "DDB_BM3D_PATH";

// The exit code with which bm3d refuses the model itself (missing, unreadable or unparseable);
// 1 is a failed build and 64 a wrong command line, both reported as a plain failure
inline constexpr int kExitInvalidInput = 2;

// Locates the bm3d executable with tools::findTool (see tool_locator.h for the search
// order); DDB_BM3D_PATH, when non-empty, is authoritative.
// Returns an empty path when no usable binary is found. The result is cached for the
// lifetime of the process; pass forceRefresh=true to bypass the cache.
fs::path findBm3dBinary(bool forceRefresh = false);

// Builds the bm3d file `output` (.bm3d) from the model `input` (OBJ, PLY or STL) by running
// `bm3d --threads N <input> <output>` as a child process: no bm3d or VCGLib code is loaded
// into the calling process. N is what the CPU pool (parallel.h) has free of the threads
// one build wants (DDB_BUILD_THREADS, build_budget.h, lowered to options.threads when the
// caller capped the build), at least 1; the tokens are held while the child runs. The
// binary is findBm3dBinary() unless `binary` is non-empty.
//
// @throws InvalidArgsException when bm3d refuses the model (exit code 2: missing,
//         unreadable or unparseable mesh or texture); the build cannot succeed until
//         the input changes.
// @throws AppException when the tool is not found (the message names bm3d and
//         DDB_BM3D_PATH), cannot start, is killed, fails the build (exit code 1) or any
//         other way, or exits with code 0 without writing `output`; the message of a
//         failed run carries the status or signal and the last 2000 bytes of what the
//         tool printed.
// @throws CanceledException when the calling thread is canceled (the child is killed).
void runBm3dBuild(const fs::path& input,
                  const fs::path& output,
                  const fs::path& binary = {},
                  const process::IProcessRunner& runner = process::systemProcessRunner(),
                  const BuildOptions& options = {});

}  // namespace bm3d

}  // namespace ddb

#endif  // BM3D_RUNNER_H
