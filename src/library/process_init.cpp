/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "process_init.h"

#include <stdexcept>
#include <clocale>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>

#include <exiv2/exiv2.hpp>
#include <sqlite3.h>

#ifdef WIN32
#include <windows.h>
#endif

#include "build_budget.h"
#include "databasepool.h"
#include "ddb.h"
#include "gdal_raii.h"
#include "hash.h"
#include "log_forwarding.h"
#include "logger.h"
#include "mio.h"
#include "raster_cache.h"
#include "diagnostics.h"

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

// A file every GDAL data folder has (it names the folder in setupGdalData)
static constexpr const char* GdalDataMarker = "gdalvrt.xsd";

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

// The folder libddb's data lookup (getDataPath: DDB_DATA, the folder of the executable, the working
// directory, the folder of the library, /usr/share/ddb, ...) finds a data file it ships in: the
// folder of `marker`, a path relative to the data folder ("proj.db", "gdal/gdalvrt.xsd"). The marker
// file, not the folder name, picks it: a "gdal" folder of the working directory that is not GDAL's
// data must not be taken. Empty when there is none.
static fs::path findDataFolder(const fs::path& marker) {
    const fs::path found = io::getDataPath(marker);
    return found.empty() ? fs::path() : found.parent_path();
}

// Whether the host set an environment variable (an empty value is none)
static bool hostSetEnv(const char* name) {
    const auto value = io::getEnvUtf8(name);
    return value.has_value() && !value->empty();
}

// PROJ's database (proj.db). PROJ looks for it in PROJ_DATA (PROJ_LIB, its legacy name), else in
// the prefix it was built for, which does not exist where the library is deployed. It ships next
// to the other data files, in a folder the host never told PROJ about (/usr/share/ddb of the
// package for /usr/bin/ddb or a dotnet host), so libddb gives it to GDAL's PROJ contexts, where
// every coordinate transformation of the library is made (OSRSetPROJSearchPaths: no environment
// variable is changed, which would have to be written in the code page on Windows). A PROJ_DATA or
// PROJ_LIB the host set wins.
//
// The argument is the folder of the executable, which the data lookup finds by itself; it stays
// for the exported signature.
void setupEnvironmentVariables(const std::string&) {
    if (hostSetEnv("PROJ_DATA") || hostSetEnv("PROJ_LIB")) {
        LOGD << "PROJ data: PROJ_DATA/PROJ_LIB of the host";
        return;
    }

    const fs::path dataFolder = findDataFolder("proj.db");
    if (dataFolder.empty()) {
        LOGW << "PROJ database (proj.db) not found next to the library";
        LOGW << "Coordinate transformations may fail";
        return;
    }

    const std::string dataDir = io::toUtf8(dataFolder);
    const char* const searchPaths[] = {dataDir.c_str(), nullptr};
    OSRSetPROJSearchPaths(searchPaths);
    LOGD << "Set the PROJ search path: " << dataDir;

    // Debug: Print proj.db hash
    try {
        const std::string projDbHash = Hash::fileSHA256(io::toUtf8(dataFolder / "proj.db"));
        LOGD << "proj.db hash: " << projDbHash << " (path: " << dataDir << ")";
    } catch (const std::exception& e) {
        LOGD << "Error computing proj.db hash: " << e.what();
    }
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

        ddb::logging::init(logToFile);
        if (enableVerbose || logToFile) {
            ddb::logging::setVerbose();
        }

    } catch (const std::exception& e) {
        std::cerr << "Failed to initialize logging: " << e.what() << std::endl;
    }
}


// GDAL's own data files (tile matrix sets, CSV tables, templates; vcpkg's share/gdal). GDAL looks
// them up through GDAL_DATA, else in the prefix it was built for, which does not exist where the
// library is deployed: every raster build then warned "Cannot find tms_NZTM2000.json
// (GDAL_DATA is not defined)". Like the other data files, they ship in a "gdal" folder next to
// the library or the executable, so a host that never sourced the environment script of the
// package (a service, Registry under dotnet) finds them too. A GDAL_DATA the host set
// (environment or CPLSetConfigOption) wins.
static void setupGdalData() {
    const char* configured = CPLGetConfigOption("GDAL_DATA", nullptr);
    if (configured != nullptr && *configured != '\0') {
        LOGD << "GDAL_DATA: " << configured;
        return;
    }

    const fs::path dataFolder = findDataFolder(fs::path("gdal") / GdalDataMarker);
    if (dataFolder.empty()) {
        LOGW << "GDAL data not found (no gdal/" << GdalDataMarker << " next to the library): "
             << "some GDAL drivers may warn about missing files";
        return;
    }
    const std::string dataDir = io::toUtf8(dataFolder);
    CPLSetConfigOption("GDAL_DATA", dataDir.c_str());
    LOGD << "Set GDAL_DATA: " << dataDir;
}

void initializeGDALandPROJ() {
    // Initialize GDAL and PROJ
    LOGD << "Initializing GDAL and PROJ libraries";

    setupGdalData();
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

            diagnostics::printVersions();
        });
    }

    void closeKeptResources()
    {
        const size_t connections = DatabasePool::instance().shutdown();
        const size_t rasters = RasterCache::instance().clear();
        LOGD << "Closed " << connections << " pooled index connections and " << rasters << " cached rasters";
    }

    void finalizeProcess()
    {
        static std::once_flag finalized;
        std::call_once(finalized, [] {
            // The cached rasters are GDAL datasets: closed by their owner, before GDAL goes
            try
            {
                closeKeptResources();
            }
            catch (const std::exception &e)
            {
                LOGW << "Cannot close what the process keeps open: " << e.what();
            }
            LOGD << "Finalizing GDAL: its worker threads are joined while they run";
            GDALDestroy();
        });
    }
}
