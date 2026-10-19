/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TOOL_LOCATOR_H
#define TOOL_LOCATOR_H

#include <string>
#include "ddb_export.h"
#include "fs.h"

namespace ddb
{
    namespace tools
    {
        // True when p is a regular file that can be executed (any exec bit on POSIX).
        DDB_DLL bool isExecutableFile(const fs::path &p);

        // Locates an external tool shipped next to DroneDB, in this order:
        //   1. envVar, when set and non-empty: authoritative (a non-executable value
        //      means "unavailable", so callers can pin or disable the tool);
        //   2. the folder of the running executable;
        //   3. <exe folder>/../bin (cmake --install layouts);
        //   4. the PATH (how hosts that are not ddb, e.g. Registry, find it).
        // baseName has no extension; ".exe" is appended on Windows. Returns an
        // empty path when the tool is not found. Not cached.
        DDB_DLL fs::path findTool(const std::string &baseName, const char *envVar);
    }
}

#endif // TOOL_LOCATOR_H
