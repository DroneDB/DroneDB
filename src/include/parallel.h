/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef PARALLEL_H
#define PARALLEL_H

#include <cstddef>
#include <functional>

#include "ddb_export.h"

namespace ddb::parallel
{
    // Cores reported by the system, at least 1.
    DDB_DLL size_t hardwareThreads();

    // Thread count from the config option `name` (environment variable or
    // CPLSetConfigOption): N > 0 is N, 0 means every core and is returned as 0
    // so callers that pass it on (GDAL "ALL_CPUS") can tell, unset or invalid
    // is min(defaultMax, cores).
    DDB_DLL int threadsOption(const char *name, int defaultMax);

    // Calls fn(i) once for every i in [0, count), on up to `maxThreads` threads
    // (the calling thread is one of them; 0 or 1 runs everything inline).
    // Indexes are handed out in increasing order. When a call throws, no new
    // index starts, the calls already running finish, and the exception of the
    // lowest failed index is rethrown: the same one a serial loop would throw.
    // GDAL errors raised by fn go to GDAL's default handler on every thread, not
    // to the process-wide handler (which PDAL replaces while its stages run).
    DDB_DLL void forEachIndex(size_t count, size_t maxThreads, const std::function<void(size_t)> &fn);

} // namespace ddb::parallel

#endif // PARALLEL_H
