/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef GDAL_FAILURE_COUNTER_H
#define GDAL_FAILURE_COUNTER_H

#include <atomic>

#include "gdal_inc.h"
#include "logger.h"

// Counts GDAL failures (CE_Failure and above) raised on any thread while in
// scope. Installs a global handler on purpose: drivers such as the MVT writer
// work on a thread pool, where a thread-local handler would miss them. Calls run
// by parallel::forEachIndex are not counted: its threads use GDAL's default
// handler (parallel.h).
class GdalFailureCounter
{
public:
    GdalFailureCounter()
    {
        failures = 0;
        previous = CPLSetErrorHandler(&count);
    }
    ~GdalFailureCounter() { CPLSetErrorHandler(previous); }
    GdalFailureCounter(const GdalFailureCounter &) = delete;
    GdalFailureCounter &operator=(const GdalFailureCounter &) = delete;

    int value() const { return failures.load(); }

private:
    static void CPL_STDCALL count(CPLErr level, CPLErrorNum, const char *msg)
    {
        if (level < CE_Failure) return;
        failures++;
        LOGD << "GDAL failure: " << (msg ? msg : "");
    }

    static inline std::atomic<int> failures{0};
    CPLErrorHandler previous = nullptr;
};

#endif // GDAL_FAILURE_COUNTER_H
