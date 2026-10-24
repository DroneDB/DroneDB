/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CRASH_HANDLERS_H
#define CRASH_HANDLERS_H

// Installs the ddb CLI's process-wide crash and termination handlers: SIGSEGV and
// SIGFPE become ddb::AppException (via segvcatch), SIGTERM/SIGINT exit, and on
// Windows an unhandled-exception filter logs the exception code.
//
// CLI only. These handlers replace whatever the process had, so libddb must never
// install them on behalf of a host: in a .NET host they turn managed
// NullReferenceException/DivideByZeroException (which the CLR raises from
// SIGSEGV/SIGFPE) into process termination, and they can replace the host's
// SIGTERM handling (DDB-11 B6).
void installCrashHandlers();

#endif // CRASH_HANDLERS_H
