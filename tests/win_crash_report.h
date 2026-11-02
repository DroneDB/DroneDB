/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef WIN_CRASH_REPORT_H
#define WIN_CRASH_REPORT_H

// Windows x64: when ddbtest dies of an unhandled SEH exception, print the
// exception code and the faulting thread's stack to stderr, then let Windows
// Error Reporting write its dump and end the process with the real code. A
// crash on a library worker thread (GDAL's thread pools) never reaches
// GoogleTest's own SEH guard, which covers only the test body's thread.
// No-op on other platforms.
void installWindowsCrashReport();

#endif // WIN_CRASH_REPORT_H
