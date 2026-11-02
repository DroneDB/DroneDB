/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The sanitizer this build is compiled with, read from the compiler: GCC defines a macro for
// each, Clang answers __has_feature. The flags of a sanitizer build apply to the whole program,
// this library included, so what is compiled here is what the process runs with.

#include "sandbox/sandbox.h"

#if defined(__SANITIZE_ADDRESS__)
#define SANDBOX_ADDRESS_SANITIZER 1
#elif defined(__SANITIZE_THREAD__)
#define SANDBOX_THREAD_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SANDBOX_ADDRESS_SANITIZER 1
#elif __has_feature(thread_sanitizer)
#define SANDBOX_THREAD_SANITIZER 1
#elif __has_feature(memory_sanitizer)
#define SANDBOX_MEMORY_SANITIZER 1
#endif
#endif

namespace sandbox
{
    SanitizerAllowances sanitizerAllowances()
    {
        SanitizerAllowances allowances;
#if defined(SANDBOX_ADDRESS_SANITIZER)
        allowances.runtime = "AddressSanitizer";
#elif defined(SANDBOX_THREAD_SANITIZER)
        allowances.runtime = "ThreadSanitizer";
#elif defined(SANDBOX_MEMORY_SANITIZER)
        allowances.runtime = "MemorySanitizer";
#endif
#ifndef _WIN32
        // Each of the three takes its shadow memory as address space
        allowances.unlimitedAddressSpace = allowances.runtime[0] != '\0';
#endif
#if defined(SANDBOX_ADDRESS_SANITIZER) && defined(__linux__)
        // LeakSanitizer is part of AddressSanitizer on Linux, where it checks at exit
        allowances.leakCheck = true;
#endif
        return allowances;
    }
} // namespace sandbox
