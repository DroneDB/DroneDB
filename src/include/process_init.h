/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef PROCESS_INIT_H
#define PROCESS_INIT_H


namespace ddb
{
    // Initializes the process for the library, once (later calls do nothing): SQLite
    // configuration (before anything initializes SQLite), PROJ data paths, locale, logging
    // (verbose, or DDB_DEBUG/DDB_LOG set: debug output), GDAL and PROJ, the XMP toolkit.
    // Call it before any other library function. It installs no signal or crash handler:
    // the host process owns them. DDBRegisterProcess (ddb.h) is the C entry point.
    void registerProcess(bool verbose = false);

    // Closes what the process keeps open between calls: a TRUNCATE checkpoint of every pooled
    // index connection, then closes them, and closes the idle cached rasters. The teardown of
    // DDBShutdown; no call may use them meanwhile.
    void closeKeptResources();

    // The last library call of an executable: on its main thread at the end of main, once no
    // other thread uses the library or GDAL; nothing may call them afterwards. Closes what the
    // process keeps open (closeKeptResources) and finalizes GDAL (GDALDestroy), which joins the
    // worker threads of GDAL's thread pool (NUM_THREADS) while they still run. Once only.
    //
    // Why (Windows): exit() runs the executable's exit-time handlers with every thread alive,
    // then ExitProcess terminates the other threads wherever they are and runs each DLL's
    // DLL_PROCESS_DETACH. GDAL's DllMain calls GDALDestroy there, which waits until the pool's
    // jobs are declared finished and locks the pool's mutex: a worker terminated after its job
    // signalled its caller but before it declared the job finished (or while it held that
    // mutex) leaves the pool waiting for it forever, and the process never exits. Once GDAL is
    // finalized, its DllMain has nothing left to do. Not part of DDBShutdown: a library host
    // may use GDAL after it, and stops long after its last libddb call returned (the workers
    // are idle by then, a state the teardown in DllMain survives).
    void finalizeProcess();
}

#endif // PROCESS_INIT_H
