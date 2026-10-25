/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef NEXUS_RUNNER_H
#define NEXUS_RUNNER_H

#include "fs.h"
#include "process_runner.h"

namespace ddb
{

    namespace nexus
    {

        // Locates the ddb-nexus executable, the separate GPL-3.0 program that writes Nexus files
        // (vendor/libnexus), with the discovery order of the other external tools:
        //   1. DDB_NEXUS_PATH env var (authoritative when non-empty)
        //   2. Folder of the current executable (next to ddbcmd/ddbtest/libddb)
        //   3. <exe_folder>/../bin (cmake --install layout)
        //   4. Directories on the system PATH (how a host such as the Registry finds it)
        // Returns an empty path when no usable binary is found. The result is cached for the
        // lifetime of the process; pass forceRefresh=true to bypass the cache.
        fs::path findNexusBinary(bool forceRefresh = false);

        // Builds the Nexus file `output` (.nxs or .nxz) from the model `input` by running
        // `ddb-nexus <input> <output>` as a child process: no Nexus or VCGLib code is loaded
        // into the calling process. The binary is findNexusBinary() unless `binary` is non-empty.
        //
        // @throws AppException when the tool is not found (the message names ddb-nexus and
        //         DDB_NEXUS_PATH), cannot start, is killed, exits with a non-zero code, or exits
        //         with code 0 without writing `output`; the message of a failed run carries the
        //         status or signal and the last 2000 bytes of what the tool printed.
        // @throws CanceledException when the calling thread is canceled (the child is killed).
        void runNexusBuild(const fs::path &input,
                           const fs::path &output,
                           const fs::path &binary = {},
                           const process::IProcessRunner &runner = process::systemProcessRunner());

    } // namespace nexus

} // namespace ddb

#endif // NEXUS_RUNNER_H
