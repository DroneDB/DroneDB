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
// thread: objects the parent's other threads (GDAL and ddb-tilers worker pools, thread-locals) still
// reference look unreachable in the child, so the check would report the parent's live
// objects as leaks and replace the child's exit code. Children that leave with _exit() never
// run the check. The parent process keeps its own leak check.
void disableLeakCheckAtExit();

// Whether this test binary is built with AddressSanitizer or ThreadSanitizer: such a build runs
// several times slower and reserves terabytes of address space for its shadow memory, so tests of
// a time budget or of an address space limit do not apply to it.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define DDB_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define DDB_TEST_SANITIZED 1
#endif
#endif
#ifndef DDB_TEST_SANITIZED
#define DDB_TEST_SANITIZED 0
#endif
inline constexpr bool kSanitizedBuild = DDB_TEST_SANITIZED != 0;

// Why a test of an address space limit is skipped in a sanitizer build
inline constexpr const char* kAddressSpaceLimitUnderSanitizer =
    "an address space limit does not fit a sanitizer build: the instrumented child reserves terabytes of shadow "
    "memory at start, so it dies from any limit, whatever it allocates";

// Skips the current test in a sanitizer build (kSanitizedBuild), saying why
#define SKIP_UNDER_SANITIZER(reason)      \
    do                                    \
    {                                     \
        if (kSanitizedBuild)              \
            GTEST_SKIP() << (reason);     \
    } while (0)

#endif // DDB_TEST_SANITIZER_HOOKS_H
