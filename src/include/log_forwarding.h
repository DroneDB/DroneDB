/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef LOG_FORWARDING_H
#define LOG_FORWARDING_H

#include <cstdint>

#include "ddb_export.h"
#include "gdal_inc.h"

// The host's log callback (DQ-15, XI-08): libddb's log records (plog) and GDAL's errors go to
// one callback the host registers (DDBSetLogCallback), with the operation id the host set on
// the thread (DDBSetThreadOperation), instead of stderr.
//
// The callback is read lock-free: an atomic pointer to an immutable registration, never freed.
// Replacing it waits until no thread is inside the previous one, so after setCallback returns
// the previous callback is never called again. A thread that is inside the callback drops the
// records it logs meanwhile (no recursion).
namespace ddb::logfwd
{
    // The levels of DDBLogLevel (ddb.h), in increasing severity
    inline constexpr int kTrace = 0;
    inline constexpr int kDebug = 1;
    inline constexpr int kInfo = 2;
    inline constexpr int kWarning = 3;
    inline constexpr int kError = 4;
    inline constexpr int kFatal = 5;

    // DDBLogCallback
    using Callback = void (*)(int level, const char *message, const char *source, uint64_t opId, void *userData);

    // Registers callback for the records of minLevel and above (kTrace..kFatal), or none
    // (nullptr). Waits until no other thread is inside the previous callback. Throws
    // InvalidArgsException for a minLevel out of range, and when called from inside the callback
    // (it would wait for itself). The first registration also makes sure the callback is
    // disabled at process exit, before libddb's exit-time handlers run.
    DDB_DLL void setCallback(Callback callback, int minLevel, void *userData);
    // setCallback(nullptr): what DDBShutdown does
    DDB_DLL void unregister();
    // The minLevel of the registered callback, or -1 when none is registered
    DDB_DLL int callbackMinLevel() noexcept;

    // Sends a record to the callback. True when the record is taken care of, so nothing else
    // should print it: a callback is registered (it took the record, or dropped it below its
    // minLevel), or this thread is running the callback (the record is dropped). False when no
    // callback is registered: print it as usual.
    DDB_DLL bool deliver(int level, const char *source, const char *message) noexcept;

    // True on a thread that is running the callback
    DDB_DLL bool inCallback() noexcept;

    // ---- the operation id of the calling thread (DDBSetThreadOperation): 0 for none

    DDB_DLL void setThreadOperation(uint64_t opId) noexcept;
    DDB_DLL uint64_t threadOperation() noexcept;

    // Sets the thread's operation id while in scope (a worker of a parallel section logs with
    // the operation of the thread that started it), then puts back the previous one
    class DDB_DLL ScopedOperation
    {
    public:
        explicit ScopedOperation(uint64_t opId) noexcept;
        ~ScopedOperation();
        ScopedOperation(const ScopedOperation &) = delete;
        ScopedOperation &operator=(const ScopedOperation &) = delete;

    private:
        uint64_t previous_;
    };

    // ---- GDAL

    // The level of a GDAL error class
    DDB_DLL int levelOf(CPLErr errorClass) noexcept;

    // A GDAL error handler that sends GDAL's messages to the callback, and to GDAL's default
    // handler (stderr) when none is registered. The failures and warnings raised for a user
    // interrupt, or while the work of the thread is canceled (cancellation.h), are the
    // consequence of the cancellation, not errors: they are sent at debug level. Installed process-wide by the process
    // initialization and pushed on every thread of a parallel section.
    DDB_DLL void CPL_STDCALL gdalErrorHandler(CPLErr errorClass, CPLErrorNum errorNum, const char *message);

    // Installs gdalErrorHandler as GDAL's process-wide handler
    DDB_DLL void installGdalErrorHandler();

} // namespace ddb::logfwd

#endif // LOG_FORWARDING_H
