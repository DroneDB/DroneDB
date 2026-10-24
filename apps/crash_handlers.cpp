/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "crash_handlers.h"

#include <csignal>
#include <cstdlib>

#ifdef WIN32
#include <windows.h>
#endif

#include "../vendor/segvcatch/segvcatch.h"
#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "version.h"

namespace {
void handleSegv();
void handleFpe();
}

void installCrashHandlers() {
    try {
        // Setup signal handlers to catch crashes and handle them gracefully
        LOGD << "Setting up signal handlers";

#ifdef WIN32
        // Windows: Setup structured exception handling

        // Configure unhandled exception filter
        SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* exceptionInfo) -> LONG {
            LOGE << "Unhandled exception detected in DDB process";
            LOGE << "Exception code: 0x" << std::hex
                 << exceptionInfo->ExceptionRecord->ExceptionCode;

            return EXCEPTION_EXECUTE_HANDLER;
        });

        LOGD << "Windows exception handler installed";

#else
        // Unix/Linux: Setup signal handlers

        // SIGSEGV handler
        signal(SIGSEGV, [](int sig) {
            LOGE << "Segmentation fault detected (SIGSEGV)";
            handleSegv();
        });

        // SIGFPE handler
        signal(SIGFPE, [](int sig) {
            LOGE << "Floating point exception detected (SIGFPE)";
            handleFpe();
        });

        // SIGTERM handler for graceful shutdown
        signal(SIGTERM, [](int sig) {
            LOGD << "Termination signal received (SIGTERM)";
            exit(0);
        });

        // SIGINT handler (Ctrl+C)
        signal(SIGINT, [](int sig) {
            LOGD << "Interrupt signal received (SIGINT)";
            exit(0);
        });

        LOGD << "Unix signal handlers installed";

#endif

        // Install existing segvcatch handlers (cross-platform)
        segvcatch::init_segv(&handleSegv);
        segvcatch::init_fpe(&handleFpe);

        LOGD << "Cross-platform crash handlers installed";

    } catch (const std::exception& e) {
        LOGW << "Failed to setup some signal handlers: " << e.what();
        LOGW << "Process may not handle crashes gracefully";
    }
}

namespace {

void handleSegv() {
    LOGE << "=== SEGMENTATION FAULT DETECTED ===";
    LOGE << "DDB Process: " << ddb::io::toUtf8(ddb::io::getExeFolderPath());
    LOGE << "Version: " << ddb::version();

    // Log process state before crash
    try {
        LOGE << "Current working directory: " << ddb::io::toUtf8(fs::current_path());
    } catch (...) {
        LOGE << "Could not determine current directory";
    }

    throw ddb::AppException("Application encountered a segfault");
}

void handleFpe() {
    LOGE << "=== FLOATING POINT EXCEPTION DETECTED ===";
    LOGE << "DDB Process: " << ddb::io::toUtf8(ddb::io::getExeFolderPath());
    LOGE << "Version: " << ddb::version();

    throw ddb::AppException("Application encountered a floating point exception");
}

}  // namespace
