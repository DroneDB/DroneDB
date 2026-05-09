/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef UNTWINE_RUNNER_H
#define UNTWINE_RUNNER_H

#include <string>
#include <vector>
#include "ddb_export.h"
#include "fs.h"

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

        // Locates the Untwine executable using the following discovery order:
        //   1. DDB_UNTWINE_PATH env var (authoritative when non-empty)
        //   2. Folder of the current executable (bundled next to ddbcmd/ddbtest)
        //   3. <exe_folder>/../bin (cmake --install layout)
        //   4. Directories on the system PATH
        // Returns an empty path if no usable binary is found. The result is cached
        // for the lifetime of the process; pass forceRefresh=true to bypass the cache.
        DDB_DLL fs::path findUntwineBinary(bool forceRefresh = false);

        // Resolves the requested backend against environment variables and binary
        // availability:
        //   - DDB_USE_PDAL_COPC=1 -> always returns Pdal
        //   - DDB_COPC_BACKEND=untwine|pdal|auto overrides the requested value
        //   - Auto -> Untwine if a binary is discoverable, otherwise Pdal
        // Throws UntwineException only when Untwine was explicitly requested and
        // the binary cannot be located.
        DDB_DLL CopcBackend resolveBackend(CopcBackend requested = CopcBackend::Auto);

        // Runs the Untwine binary to convert one or more LAS/LAZ inputs into a
        // single COPC LAZ file at outputFile. tempDir is used by Untwine for
        // intermediate files; if empty, Untwine creates one next to the output.
        // Returns true on success (binary exited with code 0 and the output file
        // exists). On failure returns false and leaves a message in errorOut.
        // The Untwine binary path is resolved via findUntwineBinary() unless
        // a non-empty path is provided.
        DDB_DLL bool runUntwine(const std::vector<std::string> &inputFiles,
                                const fs::path &outputFile,
                                const fs::path &tempDir,
                                std::string &errorOut,
                                const fs::path &binary = fs::path());

    } // namespace untwine

} // namespace ddb

#endif // UNTWINE_RUNNER_H
