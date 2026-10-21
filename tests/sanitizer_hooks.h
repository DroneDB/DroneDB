/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DDB_TEST_SANITIZER_HOOKS_H
#define DDB_TEST_SANITIZER_HOOKS_H

// Turns off LeakSanitizer's exit-time leak check for the current process (no-op in builds
// without LSan, docs/sanitizers.md).
//
// Only for a fork()ed child that must leave through std::exit() because the test needs the
// atexit handlers to run. The child has a copy of the parent's heap but only the forking
// thread: objects the parent's other threads (GDAL/PDAL worker pools, thread-locals) still
// reference look unreachable in the child, so the check would report the parent's live
// objects as leaks and replace the child's exit code. Children that leave with _exit() never
// run the check. The parent process keeps its own leak check.
void disableLeakCheckAtExit();

#endif // DDB_TEST_SANITIZER_HOOKS_H
