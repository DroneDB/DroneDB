/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-15 probe (XI-08): a host registers its log callback before DDBRegisterProcess, as Registry
// does. Runs as its own process because DDBRegisterProcess initializes once per process (the
// test binaries register in main). Exits 0 when:
//   - the DEBUG records of the initialization reach the callback (the callback's minLevel sets
//     the process log level) while the process is not verbose;
//   - a GDAL error reaches the callback with source "gdal" and the thread's operation id;
//   - the callback is never called once main has returned (libddb's exit-time handlers);
// 1 otherwise (and says why on stderr), 3 when the callback ran during the exit. The test also
// checks that nothing but "log probe ok" is printed (stdout and stderr): the callback replaces
// the console.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "ddb.h"

namespace
{
    std::atomic<bool> mainReturned{false};
    std::atomic<int> ddbDebug{0};
    std::atomic<int> gdalWithOperation{0};

    void onLog(int level, const char *message, const char *source, uint64_t opId, void *userData)
    {
        if (mainReturned)
            std::_Exit(3);
        if (userData != &ddbDebug || message == nullptr || source == nullptr)
            return;
        if (level == DDB_LOG_DEBUG && std::strcmp(source, "ddb") == 0)
            ddbDebug++;
        if (level >= DDB_LOG_WARNING && std::strcmp(source, "gdal") == 0 && opId == 77)
            gdalWithOperation++;
    }
}

int main()
{
    if (DDBSetLogCallback(&onLog, DDB_LOG_DEBUG, &ddbDebug) != DDBERR_NONE)
    {
        std::cerr << "DDBSetLogCallback before DDBRegisterProcess: " << DDBGetLastError() << std::endl;
        return 1;
    }
    if (DDBRegisterProcess(false) != DDBERR_NONE)
    {
        std::cerr << "DDBRegisterProcess: " << DDBGetLastError() << std::endl;
        return 1;
    }
    if (ddbDebug == 0)
    {
        std::cerr << "no DEBUG record of the initialization reached the callback" << std::endl;
        return 1;
    }

    // GDAL cannot open a file that does not exist: its error goes to the callback
    if (DDBSetThreadOperation(77) != DDBERR_NONE)
        return 1;
    char *out = nullptr;
    if (DDBGetRasterInfo("/no/such/raster.tif", &out) == DDBERR_NONE)
    {
        std::cerr << "DDBGetRasterInfo opened a missing raster" << std::endl;
        return 1;
    }
    if (gdalWithOperation == 0)
    {
        std::cerr << "the GDAL error did not reach the callback with the operation id" << std::endl;
        return 1;
    }
    std::cerr << "log probe ok" << std::endl;
    // From here on the exit handlers and static destructors run: no callback any more
    mainReturned = true;
    return 0;
}
