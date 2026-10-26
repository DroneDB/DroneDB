/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef UNTWINE_RUNNER_H
#define UNTWINE_RUNNER_H

#include <string>
#include <vector>
#include "fs.h"
#include "process_runner.h"
#include "tool_runner_result.h"

namespace ddb
{

    // COPC backend selection (used both for runtime decision and tests).
    enum class CopcBackend
    {
        Auto,    // discovery + fallback (default)
        Untwine, // force Untwine, throw if unavailable
        Pdal     // force PDAL writers.copc
    };

    namespace untwine
    {

        // Locates the Untwine executable with tools::findTool (see tool_locator.h for the
        // search order); DDB_UNTWINE_PATH, when non-empty, is authoritative.
        // Returns an empty path if no usable binary is found. The result is cached
        // for the lifetime of the process; pass forceRefresh=true to bypass the cache.
        fs::path findUntwineBinary(bool forceRefresh = false);

        // Resolves the requested backend against environment variables and binary
        // availability:
        //   - DDB_USE_PDAL_COPC=1 -> always returns Pdal
        //   - DDB_COPC_BACKEND=untwine|pdal|auto overrides the requested value
        //   - Auto -> Untwine if a binary is discoverable, otherwise Pdal
        // Throws UntwineException only when Untwine was explicitly requested and
        // the binary cannot be located.
        CopcBackend resolveBackend(CopcBackend requested = CopcBackend::Auto);

        // Runs the Untwine binary to convert one or more LAS/LAZ inputs into a
        // single COPC LAZ file at outputFile. tempDir is used by Untwine for
        // intermediate files; if empty, Untwine creates one next to the output.
        // The result succeeds when the binary exited with code 0 and the output file
        // exists; otherwise it carries the error. The Untwine binary path is resolved via
        // findUntwineBinary() unless a non-empty path is provided.
        ToolRunResult runUntwineResult(const std::vector<std::string> &inputFiles,
                                                 const fs::path &outputFile,
                                                 const fs::path &tempDir,
                                                 const fs::path &binary = fs::path(),
                                                 const process::Options &options = process::Options(),
                                                 const process::IProcessRunner &runner = process::systemProcessRunner());

    } // namespace untwine

} // namespace ddb

#endif // UNTWINE_RUNNER_H
