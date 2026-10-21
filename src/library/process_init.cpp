/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "process_init.h"

#include <clocale>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>

#include <exiv2/exiv2.hpp>
#include <sqlite3.h>

#ifdef WIN32
#include <windows.h>
#endif

#include "build_budget.h"
#include "ddb.h"
#include "gdal_raii.h"
#include "hash.h"
#include "log_forwarding.h"
#include "logger.h"
#include "mio.h"
#include "utils.h"

using namespace ddb;

// setupEnvironmentVariables, setupLocaleUnified, setupLogging and initializeGDALandPROJ keep
// their global names: libddb has always exported them.

// SQLite keeps allocation statistics by default (SQLITE_DEFAULT_MEMSTATUS=1), which takes a
// process-wide mutex on every allocation. Each index open parses the SpatiaLite schema
// (thousands of allocations), so concurrent opens serialized on that mutex and 8 threads
// were slower than one. Nothing here reads the statistics (sqlite3_memory_used,
// soft heap limit). Only effective before SQLite initializes.
static int configureSqlite() {
    return sqlite3_config(SQLITE_CONFIG_MEMSTATUS, 0);
}

// Thread-safe initialization using std::once_flag
static std::once_flag initialization_flag;

// Exiv2's XMP toolkit must be initialized before threads parse images (indexing runs on
// several threads, DDB-09); the lock serializes the namespaces it registers while decoding.
static void initializeXmp() {
    static std::mutex xmpMutex;
    const auto lockUnlock = [](void* data, bool lock) {
        auto* mutex = static_cast<std::mutex*>(data);
        if (lock)
            mutex->lock();
        else
            mutex->unlock();
    };
    if (!Exiv2::XmpParser::initialize(lockUnlock, &xmpMutex)) {
        LOGW << "Cannot initialize the XMP toolkit";
    }
}

void setupEnvironmentVariables(const std::string& exeFolder) {
    // Setup PROJ paths (uniform for both platforms). PROJ reads PROJ_DATA as UTF-8.
    const std::string &projDataPath = exeFolder;

    // Check for existence of proj.db
    const auto projDbPath = io::fromUtf8(projDataPath) / "proj.db";
    if (!fs::exists(projDbPath)) {
        LOGW << "PROJ database not found at: " << io::toUtf8(projDbPath);
        LOGW << "Coordinate transformations may fail";
    } else {
        LOGD << "PROJ database found at: " << io::toUtf8(projDbPath);

        // Debug: Print proj.db hash
        try {
            const std::string projDbHash = Hash::fileSHA256(io::toUtf8(projDbPath));
            LOGD << "proj.db hash: " << projDbHash << " (path: " << io::toUtf8(projDbPath) << ")";

        } catch (const std::exception& e) {
            LOGD << "Error computing proj.db hash: " << e.what();
        }
    }

#ifdef WIN32
    // Windows: uses _putenv_s for CRT synchronization

    // PROJ_DATA is the preferred (modern) variable
    if (GetEnvironmentVariableW(L"PROJ_DATA", nullptr, 0) == 0) {
        _putenv_s("PROJ_DATA", projDataPath.c_str());
        LOGD << "Set PROJ_DATA: " << projDataPath;
    }

    // PROJ_LIB is only a legacy fallback if PROJ_DATA is not present
    if (GetEnvironmentVariableW(L"PROJ_LIB", nullptr, 0) == 0 &&
        GetEnvironmentVariableW(L"PROJ_DATA", nullptr, 0) == 0) {
        _putenv_s("PROJ_LIB", projDataPath.c_str());
        LOGD << "Set PROJ_LIB (fallback): " << projDataPath;
    }

#else
    // Unix: uses setenv standard

    // PROJ_DATA is the preferred (modern) variable
    if (std::getenv("PROJ_DATA") == nullptr) {
        setenv("PROJ_DATA", projDataPath.c_str(), 1);
        LOGD << "Set PROJ_DATA: " << projDataPath;
    }

    // PROJ_LIB is only a legacy fallback if PROJ_DATA is not present
    if (std::getenv("PROJ_LIB") == nullptr && std::getenv("PROJ_DATA") == nullptr) {
        setenv("PROJ_LIB", projDataPath.c_str(), 1);
        LOGD << "Set PROJ_LIB (fallback): " << projDataPath;
    }
#endif
}

void setupLocaleUnified() {
    // Strategy: LC_ALL=C for stability, LC_CTYPE=UTF-8 for Unicode

#ifdef WIN32

    try {
        _putenv_s("LC_ALL", "C");
        std::setlocale(LC_ALL, "C");

        std::setlocale(LC_CTYPE, "en_US.UTF-8");

        LOGD << "Windows locale set: LC_ALL=C, LC_CTYPE=UTF-8";

    } catch (const std::exception& e) {
        LOGW << "Locale setup failed on Windows: " << e.what();

        std::setlocale(LC_ALL, "C");
        LOGW << "Using minimal C locale";
    }

#else

    try {
        setenv("LC_ALL", "C", 1);
        std::setlocale(LC_ALL, "C");

        // Can overwrite only LC_CTYPE for UTF-8 support
        // Try different common UTF-8 locales
        const char* utf8_locales[] = {"en_US.UTF-8", "C.UTF-8", "en_US.utf8", nullptr};

        bool utf8_set = false;
        for (const char** locale_name = utf8_locales; *locale_name; ++locale_name) {
            if (std::setlocale(LC_CTYPE, *locale_name) != nullptr) {
                LOGD << "Unix locale set: LC_ALL=C, LC_CTYPE=" << *locale_name;
                utf8_set = true;
                break;
            }
        }

        if (!utf8_set) {
            LOGW << "Could not set UTF-8 locale for LC_CTYPE, using C";
        }

    } catch (const std::exception& e) {
        LOGW << "Locale setup failed on Unix: " << e.what();
        setenv("LC_ALL", "C", 1);
        std::setlocale(LC_ALL, "C");
        LOGW << "Using minimal C locale";
    }
#endif
}


void setupLogging(bool verbose) {
    try {
        const auto logToFile = std::getenv(DDB_LOG_ENV) != nullptr;

        // Enable verbose logging if the environment variable is set
        bool enableVerbose = verbose || std::getenv(DDB_DEBUG_ENV) != nullptr;

        init_logger(logToFile);
        if (enableVerbose || logToFile) {
            set_logger_verbose();
        }

    } catch (const std::exception& e) {
        std::cerr << "Failed to initialize logging: " << e.what() << std::endl;
    }
}


void initializeGDALandPROJ() {
    // Initialize GDAL and PROJ
    LOGD << "Initializing GDAL and PROJ libraries";

    GDALAllRegister();
    // GDAL's messages go to the host's log callback when one is registered (DQ-15), else to
    // GDAL's default handler as before
    ddb::logfwd::installGdalErrorHandler();

    CPLSetConfigOption("OGR_CT_FORCE_TRADITIONAL_GIS_ORDER", "YES");
    CPLSetConfigOption("PROJ_NETWORK", "ON");

    // Disable PAM (.aux.xml) files globally - we don't need sidecar files
    CPLSetConfigOption("GDAL_PAM_ENABLED", "NO");

    // Optimize vsicurl for remote file access
    CPLSetConfigOption("GDAL_DISABLE_READDIR_ON_OPEN", "YES");
    CPLSetConfigOption("CPL_VSIL_CURL_ALLOWED_EXTENSIONS", ".tif,.tiff,.vrt,.ovr,.msk");

    // GDAL's default block cache is 5% of RAM (6 GB on a 128 GB host) and is
    // shared by tile serving and builds; bound it unless GDAL_CACHEMAX is set.
    applyDefaultGdalCacheBound();

    LOGD << "GDAL and PROJ initialization completed";

    // Check PROJ availability
    if (const SrsPtr hSRS{OSRNewSpatialReference(nullptr)}) {
        LOGD << "PROJ is working and available for coordinate transformations";
    } else {
        LOGW << "PROJ is not available, coordinate transformations may fail";
    }
}

namespace ddb
{
    void registerProcess(bool verbose)
    {
        std::call_once(initialization_flag, [verbose]() {
            // Before anything (GDAL, PROJ, SpatiaLite) initializes SQLite
            const int sqliteConfigRc = configureSqlite();

            LOGD << "Initializing DDB process";
            const auto exeFolder = io::toUtf8(io::getExeFolderPath());

            setupEnvironmentVariables(exeFolder);
            setupLocaleUnified();
            setupLogging(verbose);

            if (sqliteConfigRc != SQLITE_OK)
            {
                LOGW << "Cannot disable SQLite memory statistics (SQLite was already initialized, rc="
                     << sqliteConfigRc << "): concurrent index opens will contend on its allocator mutex";
            }
            initializeGDALandPROJ();
            // No signal or crash handlers here: this runs inside host processes (the .NET
            // Registry, tests) whose runtimes own SIGSEGV/SIGFPE/SIGTERM. The ddb CLI installs
            // its own in apps/crash_handlers.cpp.

            initializeXmp();

            // Register custom XMP namespaces for thermal sensor metadata
            try {
                Exiv2::XmpProperties::registerNs("http://ns.flir.com/xmp/1.0/", "FLIR");
                LOGD << "Registered FLIR XMP namespace";
            } catch (const Exiv2::Error &e) {
                LOGD << "FLIR XMP namespace already registered or error: " << e.what();
            }

            ddb::utils::printVersions();
        });
    }
}
