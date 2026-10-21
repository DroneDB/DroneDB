/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef PROCESS_INIT_H
#define PROCESS_INIT_H

#include "ddb_export.h"

namespace ddb
{
    // Initializes the process for the library, once (later calls do nothing): SQLite
    // configuration (before anything initializes SQLite), PROJ data paths, locale, logging
    // (verbose, or DDB_DEBUG/DDB_LOG set: debug output), GDAL and PROJ, the XMP toolkit.
    // Call it before any other library function. It installs no signal or crash handler:
    // the host process owns them. DDBRegisterProcess (ddb.h) is the C entry point.
    DDB_DLL void registerProcess(bool verbose = false);
}

#endif // PROCESS_INIT_H
