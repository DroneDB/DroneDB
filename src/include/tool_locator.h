/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TOOL_LOCATOR_H
#define TOOL_LOCATOR_H

#include <string>
#include <vector>
#include "fs.h"

namespace ddb
{
    namespace tools
    {
        // True when p is a regular file that can be executed (any exec bit on POSIX).
        bool isExecutableFile(const fs::path &p);

        // The folders findTool searches after the override variable, in order and without
        // duplicates: exeDir, <exeDir>/../bin, libDir, <libDir>/../bin. Empty inputs are skipped.
        std::vector<fs::path> searchFolders(const fs::path &exeDir, const fs::path &libDir);

        // Locates an external tool shipped next to DroneDB, in this order:
        //   1. envVar, when set and non-empty: authoritative (a non-executable value
        //      means "unavailable", so callers can pin or disable the tool);
        //   2. the folder of the running executable, then <exe folder>/../bin (cmake
        //      --install layouts);
        //   3. the folder of libddb, then <libddb folder>/../bin: how a host that loads
        //      libddb (the Registry, the .NET and Python bindings) finds the tools packaged
        //      with it, wherever the host's own executable is;
        //   4. the PATH.
        // baseName has no extension; ".exe" is appended on Windows. Returns an
        // empty path when the tool is not found.
        //
        // The answer is cached for the lifetime of the process, per tool and per value of
        // envVar (so pointing the variable somewhere else is looked up again);
        // forceRefresh=true searches again and replaces the cached answer (a tool installed
        // after the first lookup, a test that changed the PATH).
        fs::path findTool(const std::string &baseName, const char *envVar, bool forceRefresh = false);
    }
}

#endif // TOOL_LOCATOR_H
