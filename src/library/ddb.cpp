/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "ddb.h"

#include <cpr/cpr.h>

#include <csignal>
#include <mutex>

#include <exiv2/exiv2.hpp>
#include <sstream>

#ifdef WIN32
#include <windows.h>
#endif

#include "../../vendor/segvcatch/segvcatch.h"
#include "build.h"
#include "database.h"
#include "dbops.h"
#include "delta.h"
#include "entry.h"
#include "entry_types.h"
#include "exceptions.h"
#include "gdal_inc.h"
#include "hash.h"
#include "info.h"
#include "json.h"
#include "logger.h"
#include "merge_multispectral.h"
#include "mio.h"
#include "passwordmanager.h"
#include "raster_utils.h"
#include "sensorprofile.h"
#include "stac.h"
#include "status.h"
#include "syncmanager.h"
#include "tagmanager.h"
#include "thermal.h"
#include "thumbs.h"
#include "tilerhelper.h"
#include "utils.h"
#include "vegetation.h"
#include "version.h"

using namespace ddb;

char ddbLastError[255];

// Forward declarations
std::string getBuildInfo();
void handleSegv();
void handleFpe();
void setupSignalHandlers();
void setupLogging(bool verbose);
void initializeGDALandPROJ();

// Thread-safe initialization using std::once_flag
static std::once_flag initialization_flag;

void setupEnvironmentVariables(const std::string& exeFolder) {
    // Setup PROJ paths (uniform for both platforms)
    const auto projDataPath = fs::path(exeFolder).string();

    // Check for existence of proj.db
    const auto projDbPath = fs::path(projDataPath) / "proj.db";
    if (!fs::exists(projDbPath)) {
        LOGW << "PROJ database not found at: " << projDbPath.string();
        LOGW << "Coordinate transformations may fail";
    } else {
        LOGD << "PROJ database found at: " << projDbPath.string();

        // Debug: Print proj.db hash
        try {
            const std::string projDbHash = Hash::fileSHA256(projDbPath.string());
            LOGD << "proj.db hash: " << projDbHash << " (path: " << projDbPath.string() << ")";

        } catch (const std::exception& e) {
            LOGD << "Error computing proj.db hash: " << e.what();
        }
    }

#ifdef WIN32
    // Windows: uses _putenv_s for CRT synchronization

    // PROJ_DATA is the preferred (modern) variable
    if (GetEnvironmentVariableA("PROJ_DATA", nullptr, 0) == 0) {
        _putenv_s("PROJ_DATA", projDataPath.c_str());
        LOGD << "Set PROJ_DATA: " << projDataPath;
    }

    // PROJ_LIB is only a legacy fallback if PROJ_DATA is not present
    if (GetEnvironmentVariableA("PROJ_LIB", nullptr, 0) == 0 &&
        GetEnvironmentVariableA("PROJ_DATA", nullptr, 0) == 0) {
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

void setupSignalHandlers() {
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

void handleSegv() {
    LOGE << "=== SEGMENTATION FAULT DETECTED ===";
    LOGE << "DDB Process: " << io::getExeFolderPath().string();
    LOGE << "Version: " << APP_VERSION;

    // Log process state before crash
    try {
        LOGE << "Current working directory: " << fs::current_path().string();
    } catch (...) {
        LOGE << "Could not determine current directory";
    }

    throw ddb::AppException("Application encountered a segfault");
}

void handleFpe() {
    LOGE << "=== FLOATING POINT EXCEPTION DETECTED ===";
    LOGE << "DDB Process: " << io::getExeFolderPath().string();
    LOGE << "Version: " << APP_VERSION;

    throw ddb::AppException("Application encountered a floating point exception");
}

void initializeGDALandPROJ() {
    // Initialize GDAL and PROJ
    LOGD << "Initializing GDAL and PROJ libraries";

    GDALAllRegister();

    CPLSetConfigOption("OGR_CT_FORCE_TRADITIONAL_GIS_ORDER", "YES");
    CPLSetConfigOption("PROJ_NETWORK", "ON");

    // Disable PAM (.aux.xml) files globally - we don't need sidecar files
    CPLSetConfigOption("GDAL_PAM_ENABLED", "NO");

    // Optimize vsicurl for remote file access
    CPLSetConfigOption("GDAL_DISABLE_READDIR_ON_OPEN", "YES");
    CPLSetConfigOption("CPL_VSIL_CURL_ALLOWED_EXTENSIONS", ".tif,.tiff,.vrt,.ovr,.msk");

    LOGD << "GDAL and PROJ initialization completed";

    // Check PROJ availability
    OGRSpatialReferenceH hSRS = OSRNewSpatialReference(nullptr);
    if (hSRS) {
        OSRDestroySpatialReference(hSRS);
        LOGD << "PROJ is working and available for coordinate transformations";
    } else {
        LOGW << "PROJ is not available, coordinate transformations may fail";
    }
}

void DDBRegisterProcess(bool verbose) {
    std::call_once(initialization_flag, [verbose]() {
        LOGD << "Initializing DDB process";
        const auto exeFolder = io::getExeFolderPath().string();

        setupEnvironmentVariables(exeFolder);
        setupLocaleUnified();
        setupLogging(verbose);
        initializeGDALandPROJ();
        setupSignalHandlers();

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

const char* DDBGetVersion() {
    return APP_VERSION;
}

DDBErr DDBInit(const char* directory, char** outPath) {
    DDB_C_BEGIN
    if (directory == nullptr)
        throw InvalidArgsException("No directory provided");

    if (outPath == nullptr)
        throw InvalidArgsException("No output provided");

    const std::string ddbDirPath = initIndex(directory);
    utils::copyToPtr(ddbDirPath, outPath);

    DDB_C_END
}

const char* DDBGetLastError() {
    return ddbLastError;
}

void DDBSetLastError(const char* err) {
    strncpy(ddbLastError, err, 255);
    ddbLastError[254] = '\0';
}

DDBErr DDBAdd(const char* ddbPath,
              const char** paths,
              int numPaths,
              char** output,
              bool recursive) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidArrayParam(paths, numPaths))
        throw InvalidArgsException("Invalid paths array parameter");

    if (utils::isNullOrEmptyOrWhitespace(paths, numPaths))
        throw InvalidArgsException("No paths provided");

    if (utils::hasNullStringInArray(paths, numPaths))
        throw InvalidArgsException("Path array contains null elements");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    const auto db = ddb::open(std::string(ddbPath), true);
    const std::vector<std::string> pathList(paths, paths + numPaths);
    auto outJson = json::array();
    addToIndex(db.get(),
               ddb::expandPathList(pathList, recursive, 0),
               [&outJson](const Entry& e, bool) {
                   json j;
                   e.toJSON(j);
                   outJson.push_back(j);
                   return true;
               });

    utils::copyToPtr(outJson.dump(), output);
    DDB_C_END
}

DDBErr DDBRemove(const char* ddbPath, const char** paths, int numPaths) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidArrayParam(paths, numPaths))
        throw InvalidArgsException("Invalid paths array parameter");

    if (utils::isNullOrEmptyOrWhitespace(paths, numPaths))
        throw InvalidArgsException("No paths provided");

    if (utils::hasNullStringInArray(paths, numPaths))
        throw InvalidArgsException("Path array contains null elements");

    const auto db = ddb::open(std::string(ddbPath), true);
    const std::vector<std::string> pathList(paths, paths + numPaths);

    removeFromIndex(db.get(), pathList);
    DDB_C_END
}

DDBErr DDBInfo(const char** paths,
               int numPaths,
               char** output,
               const char* format,
               bool recursive,
               int maxRecursionDepth,
               const char* geometry,
               bool withHash,
               bool stopOnError) {
    DDB_C_BEGIN

    if (!utils::isValidNonEmptyStringParam(format))
        throw InvalidArgsException("No format provided");

    if (!utils::isValidNonEmptyStringParam(geometry))
        throw InvalidArgsException("No geometry provided");

    if (!utils::isValidArrayParam(paths, numPaths))
        throw InvalidArgsException("Invalid paths array parameter");

    if (utils::isNullOrEmptyOrWhitespace(paths, numPaths))
        throw InvalidArgsException("No paths provided");

    if (utils::hasNullStringInArray(paths, numPaths))
        throw InvalidArgsException("Path array contains null elements");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    if (maxRecursionDepth < 0)
        throw InvalidArgsException("Invalid max recursion depth");

    const std::vector<std::string> input(paths, paths + numPaths);
    std::ostringstream ss;
    info(input, ss, format, recursive, maxRecursionDepth, geometry, withHash, stopOnError);
    utils::copyToPtr(ss.str(), output);
    DDB_C_END
}

DDBErr DDBGet(const char* ddbPath, const char* path, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (utils::isNullOrEmptyOrWhitespace(path))
        throw InvalidArgsException("No path provided");

    const auto db = ddb::open(std::string(ddbPath), false);

    auto entries = ddb::getMatchingEntries(db.get(), std::string(path));
    std::string entryJson;
    if (entries.size() == 1) {
        entryJson = entries[0].toJSONString();
    } else if (entries.size() > 1) {
        throw InvalidArgsException("Multiple entries were returned for " + std::string(path));
    } else {
        throw InvalidArgsException("No entry " + std::string(path));
    }
    utils::copyToPtr(entryJson, output);

    DDB_C_END
}

DDBErr DDBList(const char* ddbPath,
               const char** paths,
               int numPaths,
               char** output,
               const char* format,
               bool recursive,
               int maxRecursionDepth) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidNonEmptyStringParam(format))
        throw InvalidArgsException("No format provided");

    if (!utils::isValidArrayParam(paths, numPaths))
        throw InvalidArgsException("Invalid paths array parameter");

    if (utils::isNullOrEmptyOrWhitespace(paths, numPaths))
        throw InvalidArgsException("No paths provided");

    if (utils::hasNullStringInArray(paths, numPaths))
        throw InvalidArgsException("Path array contains null elements");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    if (maxRecursionDepth < 0)
        throw InvalidArgsException("Invalid max recursion depth");

    const auto db = ddb::open(std::string(ddbPath), true);
    const std::vector<std::string> pathList(paths, paths + numPaths);

    std::ostringstream ss;
    listIndex(db.get(), pathList, ss, format, recursive, maxRecursionDepth);

    utils::copyToPtr(ss.str(), output);

    DDB_C_END
}

DDBErr DDBSearch(const char* ddbPath, const char* query, char** output, const char* format) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidStringParam(query))
        throw InvalidArgsException("No query provided");

    if (!utils::isValidNonEmptyStringParam(format))
        throw InvalidArgsException("No format provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    const auto db = ddb::open(std::string(ddbPath), false);

    std::ostringstream ss;
    searchIndex(db.get(), query, ss, format);

    utils::copyToPtr(ss.str(), output);

    DDB_C_END
}

DDBErr DDBAppendPassword(const char* ddbPath, const char* password) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidNonEmptyStringParam(password))
        throw InvalidArgsException("No password provided");

    const auto db = ddb::open(std::string(ddbPath), true);

    PasswordManager manager(db.get());

    manager.append(std::string(password));

    DDB_C_END
}

DDBErr DDBVerifyPassword(const char* ddbPath, const char* password, bool* verified) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    // We allow empty password verification
    if (password == nullptr)
        throw InvalidArgsException("No password provided");

    if (verified == nullptr)
        throw InvalidArgsException("Output parameter pointer is null");

    const auto db = ddb::open(std::string(ddbPath), true);

    PasswordManager manager(db.get());

    *verified = manager.verify(std::string(password));

    DDB_C_END
}

DDBErr DDBClearPasswords(const char* ddbPath) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    const auto db = ddb::open(std::string(ddbPath), true);

    PasswordManager manager(db.get());

    manager.clearAll();

    DDB_C_END
}

DDB_DLL DDBErr DDBStatus(const char* ddbPath, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (output == nullptr)
        throw InvalidArgsException("No output provided");

    const auto db = ddb::open(std::string(ddbPath), true);

    std::ostringstream ss;

    const auto cb = [&ss](ddb::FileStatus status, const std::string&) {
        switch (status) {
            case NotIndexed:
                ss << "?\t";
                break;
            case Deleted:
                ss << "!\t";
                break;
            case Modified:
                ss << "M\t";
                break;
            case NotModified:
                break;
        }
    };

    statusIndex(db.get(), cb);

    utils::copyToPtr(ss.str(), output);

    DDB_C_END
}

// @deprecated
// @deprecated
DDBErr DDBChattr(const char* ddbPath, const char* attrsJson, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidStringParam(attrsJson))
        throw InvalidArgsException("No attributes JSON provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    LOGD << "Deprecated DDBChattr call: please use DDBMetaSet instead as DDBChattr will be removed "
            "in the near future.";

    const auto db = ddb::open(std::string(ddbPath), true);
    try {
        const json j = json::parse(attrsJson);
        for (auto& el : j.items()) {
            if (el.key() == "public" && el.value().is_boolean()) {
                db->getMetaManager()->set("visibility", el.value() ? "1" : "0");
            } else {
                throw InvalidArgsException("Invalid attribute " + el.key());
            }
        }
        utils::copyToPtr(db->getProperties().dump(), output);
    } catch (const json::parse_error& e) {
        throw InvalidArgsException(e.what());
    }
    DDB_C_END
}

DDBErr DDBGenerateThumbnail(const char* filePath, int size, const char* destPath) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(filePath))
        throw InvalidArgsException("No file path provided");

    if (utils::isNullOrEmptyOrWhitespace(destPath))
        throw InvalidArgsException("No destination path provided");

    if (size < 0)
        throw InvalidArgsException("Invalid size parameter");

    const auto imagePath = fs::path(filePath);
    const auto thumbPath = fs::path(destPath);

    generateThumb(imagePath, size, thumbPath, true, nullptr, nullptr);

    DDB_C_END
}

DDB_DLL DDBErr DDBGenerateMemoryThumbnail(const char* filePath,
                                          int size,
                                          uint8_t** outBuffer,
                                          int* outBufferSize) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(filePath))
        throw InvalidArgsException("No file path provided");

    if (size < 0)
        throw InvalidArgsException("Invalid size parameter");

    if (outBuffer == nullptr)
        throw InvalidArgsException("Output buffer pointer is null");

    if (outBufferSize == nullptr)
        throw InvalidArgsException("Output buffer size pointer is null");

    const auto imagePath = fs::path(filePath);

    generateThumb(imagePath, size, "", true, outBuffer, outBufferSize);

    DDB_C_END
}

DDBErr DDBVSIFree(uint8_t* buffer) {
    DDB_C_BEGIN

    if (buffer == nullptr)
        throw InvalidArgsException("Buffer pointer is null");

    VSIFree(buffer);
    DDB_C_END
}

DDBErr DDBFree(char* ptr) {
    DDB_C_BEGIN
    // ptr == nullptr is allowed (no-op, matches standard free behaviour)
    free(ptr);
    DDB_C_END
}

DDB_DLL DDBErr DDBTile(const char* inputPath,
                       int tz,
                       int tx,
                       int ty,
                       char** outputTilePath,
                       int tileSize,
                       bool tms,
                       bool forceRecreate) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(inputPath))
        throw InvalidArgsException("No input path provided");

    if (outputTilePath == nullptr)
        throw InvalidArgsException("Output tile path pointer is null");

    if (tileSize < 0)
        throw InvalidArgsException("Invalid tile size parameter");

    if (tz < 0 || tx < 0 || ty < 0)
        throw InvalidArgsException("Invalid tile coordinates");

    utils::copyToPtr("", outputTilePath);
    const auto tilePath = ddb::TilerHelper::getFromUserCache(std::string(inputPath),
                                                             tz,
                                                             tx,
                                                             ty,
                                                             tileSize,
                                                             tms,
                                                             forceRecreate);
    utils::copyToPtr(tilePath.string(), outputTilePath);
    DDB_C_END
}

DDBErr DDBMemoryTile(const char* inputPath,
                     int tz,
                     int tx,
                     int ty,
                     uint8_t** outBuffer,
                     int* outBufferSize,
                     int tileSize,
                     bool tms,
                     bool forceRecreate,
                     const char* inputPathHash) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(inputPath))
        throw InvalidArgsException("No input path provided");

    if (outBuffer == nullptr)
        throw InvalidArgsException("Output buffer pointer is null");

    if (outBufferSize == nullptr)
        throw InvalidArgsException("Output buffer size pointer is null");

    if (tileSize < 0)
        throw InvalidArgsException("Invalid tile size parameter");

    if (tz < 0 || tx < 0 || ty < 0)
        throw InvalidArgsException("Invalid tile coordinates");

    // inputPathHash can be null or empty, it's optional
    const std::string hashStr = inputPathHash ? std::string(inputPathHash) : "";

    ddb::TilerHelper::getTile(std::string(inputPath),
                              tz,
                              tx,
                              ty,
                              tileSize,
                              tms,
                              forceRecreate,
                              "",
                              outBuffer,
                              outBufferSize,
                              hashStr);
    DDB_C_END
}

DDBErr DDBDelta(const char* ddbSourceStamp,
                const char* ddbTargetStamp,
                char** output,
                const char* format) {
    DDB_C_BEGIN

    if (ddbSourceStamp == nullptr)
        throw InvalidArgsException("No ddb source path provided");

    if (ddbTargetStamp == nullptr)
        throw InvalidArgsException("No ddb path provided");

    if (format == nullptr || strlen(format) == 0)
        throw InvalidArgsException("No format provided");

    if (output == nullptr)
        throw InvalidArgsException("No output provided");

    std::ostringstream ss;

    try {
        const json source = json::parse(ddbSourceStamp);
        const json dest = json::parse(ddbTargetStamp);

        delta(source, dest, ss, format);

        utils::copyToPtr(ss.str(), output);
    } catch (const json::parse_error& e) {
        throw InvalidArgsException(e.what());
    }

    DDB_C_END
}

DDB_DLL DDBErr DDBApplyDelta(const char* delta,
                             const char* sourcePath,
                             const char* ddbPath,
                             int mergeStrategy,
                             const char* sourceMetaDump,
                             char** conflicts) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(delta))
        throw InvalidArgsException("No delta provided");

    if (utils::isNullOrEmptyOrWhitespace(sourcePath))
        throw InvalidArgsException("No source path provided");

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No ddb path provided");

    if (utils::isNullOrEmptyOrWhitespace(sourceMetaDump))
        throw InvalidArgsException("No source meta dump provided");

    if (conflicts == nullptr)
        throw InvalidArgsException("Conflicts output pointer is null");

    if (mergeStrategy < 0)
        throw InvalidArgsException("Invalid merge strategy");

    Delta d;
    json metaDump;
    try {
        d = json::parse(delta);
        metaDump = json::parse(sourceMetaDump);
    } catch (const json::parse_error& e) {
        throw InvalidArgsException(e.what());
    }

    const auto ddb = ddb::open(std::string(ddbPath), false);
    std::stringstream ss;
    const auto adc = applyDelta(d,
                                fs::path(std::string(sourcePath)),
                                ddb.get(),
                                static_cast<MergeStrategy>(mergeStrategy),
                                metaDump,
                                ss);

    json j = json::array();
    for (auto& c : adc)
        j.push_back(c.path);
    utils::copyToPtr(j.dump(), conflicts);

    DDB_C_END
}

DDBErr DDBComputeDeltaLocals(const char* delta,
                             const char* ddbPath,
                             const char* hlDestFolder,
                             char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(delta))
        throw InvalidArgsException("No delta provided");

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    // hlDestFolder can be empty string, but not null
    if (hlDestFolder == nullptr)
        throw InvalidArgsException("Destination folder parameter is null");

    Delta d;
    try {
        d = json::parse(delta);
    } catch (const json::parse_error& e) {
        throw InvalidArgsException(e.what());
    }
    const auto ddb = ddb::open(std::string(ddbPath), false);

    auto cdl = computeDeltaLocals(d, ddb.get(), std::string(hlDestFolder));
    json j = json::object();
    for (auto& el : cdl)
        j[el.first] = el.second;
    utils::copyToPtr(j.dump(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBSetTag(const char* ddbPath, const char* newTag) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No ddb path provided");

    if (!utils::isValidStringParam(newTag))
        throw InvalidArgsException("No tag provided");

    const auto ddb = ddb::open(std::string(ddbPath), true);
    TagManager manager(ddb.get());
    manager.setTag(newTag);

    DDB_C_END
}

DDB_DLL DDBErr DDBGetTag(const char* ddbPath, char** outTag) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (outTag == nullptr)
        throw InvalidArgsException("Output tag pointer is null");

    const auto ddb = ddb::open(std::string(ddbPath), true);
    TagManager manager(ddb.get());
    const auto tag = manager.getTag();

    utils::copyToPtr(tag, outTag);

    DDB_C_END
}

DDB_DLL DDBErr DDBGetStamp(const char* ddbPath, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (output == nullptr)
        throw InvalidArgsException("No output provided");

    const auto ddb = ddb::open(std::string(ddbPath), true);
    utils::copyToPtr(ddb->getStamp().dump(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBRescan(const char* ddbPath, char** output, const char* types, bool stopOnError) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    // Parse types string into vector
    const std::vector<EntryType> typeFilter = utils::parseEntryTypeList(types);

    const auto db = ddb::open(std::string(ddbPath), true);
    auto outJson = json::array();

    rescanIndex(db.get(), typeFilter, stopOnError,
                [&outJson](const Entry& e, bool success, const std::string& error) {
                    json j;
                    j["path"] = e.path;
                    j["success"] = success;
                    if (!success)
                        j["error"] = error;
                    if (success)
                        e.toJSON(j);
                    outJson.push_back(j);
                    return true; // continue processing
                });

    utils::copyToPtr(outJson.dump(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBMoveEntry(const char* ddbPath, const char* source, const char* dest) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (utils::isNullOrEmptyOrWhitespace(source))
        throw InvalidArgsException("No source path provided");
    if (utils::isNullOrEmptyOrWhitespace(dest))
        throw InvalidArgsException("No dest path provided");

    const auto ddb = ddb::open(std::string(ddbPath), true);

    moveEntry(ddb.get(), std::string(source), std::string(dest));

    DDB_C_END
}

DDB_DLL DDBErr
DDBBuild(const char* ddbPath, const char* source, const char* dest, bool force, bool pendingOnly) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No ddb path provided");

    const auto ddb = ddb::open(std::string(ddbPath), true);

    // dest can be null (optional parameter)
    const auto destPath = dest != nullptr ? std::string(dest) : "";

    // source can be null (optional parameter)
    std::string path;
    if (source != nullptr)
        path = std::string(source);

    try {
        if (path.empty()) {
            if (pendingOnly)
                buildPending(ddb.get(), destPath, force);
            else
                buildAll(ddb.get(), destPath, force);
        } else {
            build(ddb.get(), path, destPath, force);
        }
    } catch (const ddb::BuildDepMissingException& e) {
        return DDBERR_BUILDDEPMISSING;
    }

    DDB_C_END
}

DDB_DLL DDBErr DDBIsBuildable(const char* ddbPath, const char* path, bool* isBuildable) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidStringParam(path))
        throw InvalidArgsException("No path provided");

    if (isBuildable == nullptr)
        throw InvalidArgsException("Buildable parameter is null");

    const auto ddbPathStr = std::string(ddbPath);

    const auto ddb = ddb::open(ddbPathStr, true);

    std::string subfolder;

    *isBuildable = ddb::isBuildable(ddb.get(), std::string(path), subfolder);

    DDB_C_END
}

DDB_DLL DDBErr DDBIsBuildPending(const char* ddbPath, bool* isBuildPending) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (isBuildPending == nullptr)
        throw InvalidArgsException("isBuildPending parameter is null");

    const auto ddb = ddb::open(std::string(ddbPath), true);
    *isBuildPending = ddb::isBuildPending(ddb.get());

    DDB_C_END
}

DDB_DLL DDBErr DDBIsBuildActive(const char* ddbPath, const char* path, bool* isBuildActive) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (utils::isNullOrEmptyOrWhitespace(path))
        throw InvalidArgsException("No path provided");

    if (isBuildActive == nullptr)
        throw InvalidArgsException("isBuildActive parameter is null");

    const auto ddb = ddb::open(std::string(ddbPath), true);
    *isBuildActive = ddb::isBuildActive(ddb.get(), std::string(path));

    DDB_C_END
}

DDBErr DDBMetaAdd(const char* ddbPath,
                  const char* path,
                  const char* key,
                  const char* data,
                  char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidStringParam(path))
        throw InvalidArgsException("No path provided");

    if (!utils::isValidStringParam(key))
        throw InvalidArgsException("No key provided");

    if (!utils::isValidStringParam(data))
        throw InvalidArgsException("No data provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    const auto ddbPathStr = std::string(ddbPath);

    const auto ddb = ddb::open(ddbPathStr, true);
    auto json = ddb->getMetaManager()->add(std::string(key),
                                           std::string(data),
                                           std::string(path),
                                           ddbPathStr);

    utils::copyToPtr(json.dump(), output);

    DDB_C_END
}

DDBErr DDBMetaSet(const char* ddbPath,
                  const char* path,
                  const char* key,
                  const char* data,
                  char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidStringParam(path))
        throw InvalidArgsException("No path provided");

    if (!utils::isValidStringParam(key))
        throw InvalidArgsException("No key provided");

    if (!utils::isValidStringParam(data))
        throw InvalidArgsException("No data provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    const auto ddbPathStr = std::string(ddbPath);

    const auto ddb = ddb::open(ddbPathStr, true);
    auto json = ddb->getMetaManager()->set(std::string(key),
                                           std::string(data),
                                           std::string(path),
                                           ddbPathStr);

    utils::copyToPtr(json.dump(), output);

    DDB_C_END
}

DDBErr DDBMetaRemove(const char* ddbPath, const char* id, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidStringParam(id))
        throw InvalidArgsException("No id provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    const auto ddb = ddb::open(std::string(ddbPath), true);
    auto json = ddb->getMetaManager()->remove(std::string(id));

    utils::copyToPtr(json.dump(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBMetaGet(const char* ddbPath, const char* path, const char* key, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidStringParam(path))
        throw InvalidArgsException("No path provided");

    if (!utils::isValidStringParam(key))
        throw InvalidArgsException("No key provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    const auto ddbPathStr = std::string(ddbPath);

    const auto ddb = ddb::open(ddbPathStr, true);
    auto json = ddb->getMetaManager()->get(std::string(key), std::string(path), ddbPathStr);

    utils::copyToPtr(json.dump(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBMetaUnset(const char* ddbPath, const char* path, const char* key, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidStringParam(path))
        throw InvalidArgsException("No path provided");

    if (!utils::isValidStringParam(key))
        throw InvalidArgsException("No key provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    const auto ddb = ddb::open(std::string(ddbPath), true);
    auto json =
        ddb->getMetaManager()->unset(std::string(key), std::string(path), std::string(ddbPath));

    utils::copyToPtr(json.dump(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBMetaList(const char* ddbPath, const char* path, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidStringParam(path))
        throw InvalidArgsException("No path provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    const auto ddbPathStr = std::string(ddbPath);

    const auto ddb = ddb::open(ddbPathStr, true);
    auto json = ddb->getMetaManager()->list(std::string(path), ddbPathStr);

    utils::copyToPtr(json.dump(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBMetaDump(const char* ddbPath, const char* ids, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidStringParam(ids))
        throw InvalidArgsException("No ids provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    json jIds;
    try {
        jIds = json::parse(ids);
    } catch (const json::parse_error& e) {
        throw InvalidArgsException(e.what());
    }

    const auto ddb = ddb::open(std::string(ddbPath), true);
    auto json = ddb->getMetaManager()->dump(jIds);

    utils::copyToPtr(json.dump(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBMetaRestore(const char* ddbPath, const char* dump, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (!utils::isValidStringParam(dump))
        throw InvalidArgsException("No dump provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    json jDump;
    try {
        jDump = json::parse(dump);
    } catch (const json::parse_error& e) {
        throw InvalidArgsException(e.what());
    }

    const auto ddb = ddb::open(std::string(ddbPath), true);
    auto json = ddb->getMetaManager()->restore(jDump);

    utils::copyToPtr(json.dump(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBStac(const char* ddbPath,
                       const char* entry,
                       const char* stacCollectionRoot,
                       const char* id,
                       const char* stacCatalogRoot,
                       char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(ddbPath))
        throw InvalidArgsException("No directory provided");

    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    // entry, stacCollectionRoot, id, stacCatalogRoot can be null/empty - they are optional
    // parameters
    const std::string entryStr = entry ? std::string(entry) : "";
    const std::string stacCollectionRootStr =
        stacCollectionRoot ? std::string(stacCollectionRoot) : "";
    const std::string idStr = id ? std::string(id) : "";
    const std::string stacCatalogRootStr = stacCatalogRoot ? std::string(stacCatalogRoot) : "";

    const auto ddb = ddb::open(std::string(ddbPath), false);
    auto json = ddb::generateStac(std::string(ddbPath),
                                  entryStr,
                                  stacCollectionRootStr,
                                  idStr,
                                  stacCatalogRootStr);

    utils::copyToPtr(json.dump(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBGetRasterInfo(const char* path, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(path))
        throw InvalidArgsException("No path provided");
    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    auto& spm = ddb::SensorProfileManager::instance();
    std::string jsonStr = spm.getRasterInfoJson(std::string(path));
    utils::copyToPtr(jsonStr, output);

    DDB_C_END
}

DDB_DLL DDBErr DDBGetRasterMetadata(const char* path, const char* formula,
                                     const char* bandFilter, char** output) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(path))
        throw InvalidArgsException("No path provided");
    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    const std::string formulaStr = formula ? std::string(formula) : "";
    const std::string filterStr = bandFilter ? std::string(bandFilter) : "";

    GDALDatasetH hDs = GDALOpen(path, GA_ReadOnly);
    if (!hDs)
        throw GDALException("Cannot open " + std::string(path));

    auto& ve = ddb::VegetationEngine::instance();
    json result;

    int nBands = GDALGetRasterCount(hDs);

    // Determine effective band filter
    ddb::BandFilter effectiveFilter;
    if (!filterStr.empty()) {
        effectiveFilter = ddb::VegetationEngine::parseFilter(filterStr, nBands);
    } else {
        effectiveFilter = ve.autoDetectFilter(std::string(path));
    }

    // Statistics
    json statsJson;

    if (!formulaStr.empty()) {
        // Formula statistics: compute on-demand
        const ddb::BandFilter &bf = effectiveFilter;
        int width = GDALGetRasterXSize(hDs);
        int height = GDALGetRasterYSize(hDs);

        // Sample at reduced resolution for performance
        int sampleW = std::min(width, 512);
        int sampleH = std::min(height, 512);
        size_t pixCount = static_cast<size_t>(sampleW) * sampleH;

        std::vector<std::vector<float>> bandDataStorage(nBands);
        std::vector<float*> bandDataPtrs(nBands);

        // Read GDAL nodata values and detect alpha band
        auto nodataInfo = detectBandNodata(hDs, nBands);
        for (int b = 0; b < nBands; b++) {
            bandDataStorage[b].resize(pixCount);
            bandDataPtrs[b] = bandDataStorage[b].data();
            GDALRasterBandH hBand = GDALGetRasterBand(hDs, b + 1);
            GDALRasterIO(hBand, GF_Read, 0, 0, width, height,
                         bandDataPtrs[b], sampleW, sampleH, GDT_Float32, 0, 0);
        }

        // Pre-mask: set pixels to output nodata where alpha=0 or any band
        // matches its GDAL nodata value, so applyFormula excludes them
        float nodata = NODATA_SENTINEL;
        premaskNodata(bandDataPtrs, pixCount, nBands, nodataInfo, nodata);

        std::vector<float> formulaResult(pixCount);
        const auto* formulaObj = ve.getFormula(formulaStr);
        if (!formulaObj) {
            GDALClose(hDs);
            throw InvalidArgsException("Unknown formula: " + formulaStr);
        }
        ve.applyFormula(*formulaObj, bf, bandDataPtrs, formulaResult.data(), pixCount, nodata);

        // Compute statistics
        std::vector<double> valid;
        valid.reserve(pixCount);
        double sum = 0, sumSq = 0;
        double fMin = std::numeric_limits<double>::max();
        double fMax = std::numeric_limits<double>::lowest();
        for (size_t i = 0; i < pixCount; i++) {
            if (formulaResult[i] != nodata) {
                double v = static_cast<double>(formulaResult[i]);
                valid.push_back(v);
                sum += v;
                sumSq += v * v;
                fMin = std::min(fMin, v);
                fMax = std::max(fMax, v);
            }
        }

        if (!valid.empty()) {
            double mean = sum / valid.size();
            double variance = (sumSq / valid.size()) - (mean * mean);
            double stddev = std::sqrt(std::max(0.0, variance));

            std::sort(valid.begin(), valid.end());
            double p2 = valid[static_cast<size_t>(valid.size() * 0.02)];
            double p98 = valid[std::min(valid.size() - 1, static_cast<size_t>(valid.size() * 0.98))];

            // Histogram
            int bins = 255;
            std::vector<int> counts(bins, 0);
            std::vector<double> edges(bins + 1);
            double range = fMax - fMin;
            if (range <= 0) range = 1.0;
            for (int i = 0; i <= bins; i++) {
                edges[i] = fMin + (range * i / bins);
            }
            for (double v : valid) {
                int idx = static_cast<int>((v - fMin) / range * (bins - 1));
                idx = std::max(0, std::min(bins - 1, idx));
                counts[idx]++;
            }

            statsJson["1"] = {
                {"min", fMin}, {"max", fMax}, {"mean", mean}, {"std", stddev},
                {"percentiles", {{"p2", p2}, {"p98", p98}}},
                {"histogram", {{"counts", counts}, {"edges", edges}, {"bins", bins}}}
            };
        }
    } else {
        // Per-band statistics
        for (int i = 1; i <= nBands; i++) {
            GDALRasterBandH hBand = GDALGetRasterBand(hDs, i);
            double bMin, bMax, bMean, bStdDev;
            if (GDALComputeRasterStatistics(hBand, TRUE, &bMin, &bMax, &bMean, &bStdDev, nullptr, nullptr) == CE_None) {
                statsJson[std::to_string(i)] = {
                    {"min", bMin}, {"max", bMax}, {"mean", bMean}, {"std", bStdDev},
                    {"percentiles", {{"p2", bMean - 2.33 * bStdDev}, {"p98", bMean + 2.33 * bStdDev}}}
                };
            }
        }
    }

    result["statistics"] = statsJson;

    // Available algorithms for this band filter
    // Include all formulas, mark incompatible ones as disabled
    auto compatibleFormulas = ve.getFormulasForFilter(effectiveFilter);
    auto allFormulas = ve.getAllFormulas();
    json algJson = json::array();
    for (const auto& f : allFormulas) {
        json fj = {
            {"id", f.id}, {"name", f.name}, {"expr", f.expr},
            {"help", f.help}
        };
        if (f.hasRange) {
            fj["range"] = {f.rangeMin, f.rangeMax};
        }
        bool compatible = false;
        for (const auto& cf : compatibleFormulas) {
            if (cf.id == f.id) { compatible = true; break; }
        }
        if (!compatible) {
            fj["disabled"] = true;
            fj["disabledReason"] = "Requires bands: " + f.requiredBands;
        }
        algJson.push_back(fj);
    }
    result["algorithms"] = algJson;

    // Colormaps
    result["colormaps"] = ve.getColormapsJson();

    result["autoBands"] = {{"filter", effectiveFilter.id}, {"match", !effectiveFilter.id.empty()}};

    auto& spm = ddb::SensorProfileManager::instance();
    auto detection = spm.detectSensor(std::string(path));
    result["detectedSensor"] = detection.sensorId;

    GDALClose(hDs);

    utils::copyToPtr(result.dump(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBGenerateMemoryThumbnailEx(const char* filePath, int size,
                                             const char* preset, const char* bands,
                                             const char* formula, const char* bandFilter,
                                             const char* colormap, const char* rescale,
                                             uint8_t** outBuffer, int* outBufferSize) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(filePath))
        throw InvalidArgsException("No file path provided");
    if (size < 0)
        throw InvalidArgsException("Invalid size parameter");
    if (outBuffer == nullptr)
        throw InvalidArgsException("Output buffer pointer is null");
    if (outBufferSize == nullptr)
        throw InvalidArgsException("Output buffer size pointer is null");

    ddb::ThumbVisParams visParams;
    if (preset) visParams.preset = std::string(preset);
    if (bands) visParams.bands = std::string(bands);
    if (formula) visParams.formula = std::string(formula);
    if (bandFilter) visParams.bandFilter = std::string(bandFilter);
    if (colormap) visParams.colormap = std::string(colormap);
    if (rescale) visParams.rescale = std::string(rescale);

    ddb::generateImageThumbEx(fs::path(filePath), size, "", outBuffer, outBufferSize, visParams);

    DDB_C_END
}

DDB_DLL DDBErr DDBMemoryTileEx(const char* inputPath,
                                int tz, int tx, int ty,
                                int tileSize, bool tms,
                                bool forceRecreate, const char* inputPathHash,
                                const char* preset, const char* bands,
                                const char* formula, const char* bandFilter,
                                const char* colormap, const char* rescale,
                                uint8_t** outBuffer, int* outBufferSize) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(inputPath))
        throw InvalidArgsException("No input path provided");
    if (outBuffer == nullptr)
        throw InvalidArgsException("Output buffer pointer is null");
    if (outBufferSize == nullptr)
        throw InvalidArgsException("Output buffer size pointer is null");
    if (tileSize < 0)
        throw InvalidArgsException("Invalid tile size parameter");
    if (tz < 0 || tx < 0 || ty < 0)
        throw InvalidArgsException("Invalid tile coordinates");

    // Build vizKey from params for cache differentiation
    std::string vizKey;
    if (preset) vizKey += std::string("p:") + preset;
    if (bands) vizKey += std::string(",b:") + bands;
    if (formula) vizKey += std::string(",f:") + formula;
    if (bandFilter) vizKey += std::string(",bf:") + bandFilter;
    if (colormap) vizKey += std::string(",cm:") + colormap;
    if (rescale) vizKey += std::string(",rs:") + rescale;

    const std::string hashStr = inputPathHash ? std::string(inputPathHash) : "";

    ddb::ThumbVisParams visParams;
    if (preset) visParams.preset = std::string(preset);
    if (bands) visParams.bands = std::string(bands);
    if (formula) visParams.formula = std::string(formula);
    if (bandFilter) visParams.bandFilter = std::string(bandFilter);
    if (colormap) visParams.colormap = std::string(colormap);
    if (rescale) visParams.rescale = std::string(rescale);

    ddb::TilerHelper::getTile(std::string(inputPath),
                              tz, tx, ty,
                              tileSize, tms,
                              forceRecreate,
                              "",
                              visParams,
                              outBuffer, outBufferSize,
                              hashStr + vizKey);

    DDB_C_END
}

DDB_DLL DDBErr DDBValidateMergeMultispectral(const char** paths, int numPaths, char** output) {
    DDB_C_BEGIN

    if (paths == nullptr || numPaths < 1)
        throw InvalidArgsException("No input paths provided");
    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    std::vector<std::string> inputPaths;
    for (int i = 0; i < numPaths; i++) {
        if (paths[i]) inputPaths.emplace_back(paths[i]);
    }

    auto validationResult = ddb::validateMergeMultispectral(inputPaths);

    json j;
    j["ok"] = validationResult.ok;
    j["errors"] = validationResult.errors;
    j["warnings"] = validationResult.warnings;
    j["summary"] = {
        {"totalBands", validationResult.summary.totalBands},
        {"dataType", validationResult.summary.dataType},
        {"width", validationResult.summary.width},
        {"height", validationResult.summary.height},
        {"crs", validationResult.summary.crs},
        {"pixelSizeX", validationResult.summary.pixelSizeX},
        {"pixelSizeY", validationResult.summary.pixelSizeY},
        {"estimatedSize", validationResult.summary.estimatedSize}
    };

    // Alignment info
    json alignmentJson;
    alignmentJson["detected"] = validationResult.alignment.detected;
    alignmentJson["maxShiftPixels"] = validationResult.alignment.maxShiftPixels;
    alignmentJson["correctionApplied"] = validationResult.alignment.correctionApplied;
    alignmentJson["shiftSource"] = validationResult.alignment.shiftSource;

    json bandsJson = json::array();
    for (size_t i = 0; i < validationResult.alignment.bands.size(); i++) {
        const auto &b = validationResult.alignment.bands[i];
        bandsJson.push_back({
            {"index", i},
            {"name", b.bandName},
            {"wavelength", b.centralWavelength},
            {"shiftX", b.shiftX},
            {"shiftY", b.shiftY}
        });
    }
    alignmentJson["bands"] = bandsJson;
    j["alignment"] = alignmentJson;

    utils::copyToPtr(j.dump(), output);

    DDB_C_END
}

DDB_DLL DDBErr DDBPreviewMergeMultispectral(const char** paths, int numPaths,
                                             const char* previewBands, int thumbSize,
                                             uint8_t** outBuffer, int* outBufferSize) {
    DDB_C_BEGIN

    if (paths == nullptr || numPaths < 1)
        throw InvalidArgsException("No input paths provided");
    if (outBuffer == nullptr)
        throw InvalidArgsException("Output buffer pointer is null");
    if (outBufferSize == nullptr)
        throw InvalidArgsException("Output buffer size pointer is null");

    std::vector<std::string> inputPaths;
    for (int i = 0; i < numPaths; i++) {
        if (paths[i]) inputPaths.emplace_back(paths[i]);
    }

    std::vector<int> bandsVec;
    if (previewBands) {
        std::istringstream ss(previewBands);
        std::string token;
        while (std::getline(ss, token, ',')) {
            try {
                bandsVec.push_back(std::stoi(token));
            } catch (const std::exception&) {
                throw InvalidArgsException("Invalid preview band value: '" + token + "'");
            }
        }
    }
    if (bandsVec.size() < 3) {
        bandsVec = {1, 2, 3};
    }

    ddb::previewMergeMultispectral(inputPaths, bandsVec, thumbSize > 0 ? thumbSize : 512, outBuffer, outBufferSize);

    DDB_C_END
}

DDB_DLL DDBErr DDBMergeMultispectral(const char** paths, int numPaths, const char* outputPath) {
    DDB_C_BEGIN

    if (paths == nullptr || numPaths < 2)
        throw InvalidArgsException("At least 2 input paths required");
    if (utils::isNullOrEmptyOrWhitespace(outputPath))
        throw InvalidArgsException("No output path provided");

    std::vector<std::string> inputPaths;
    for (int i = 0; i < numPaths; i++) {
        if (paths[i]) inputPaths.emplace_back(paths[i]);
    }

    ddb::mergeMultispectral(inputPaths, std::string(outputPath));

    DDB_C_END
}

DDB_DLL DDBErr DDBExportRaster(const char* inputPath, const char* outputPath,
                                const char* preset, const char* bands,
                                const char* formula, const char* bandFilter,
                                const char* colormap, const char* rescale) {
    DDB_C_BEGIN

    if (utils::isNullOrEmptyOrWhitespace(inputPath))
        throw InvalidArgsException("No input path provided");
    if (utils::isNullOrEmptyOrWhitespace(outputPath))
        throw InvalidArgsException("No output path provided");

    GDALDatasetH hSrcDs = GDALOpen(inputPath, GA_ReadOnly);
    if (!hSrcDs)
        throw GDALException("Cannot open " + std::string(inputPath));

    const int width = GDALGetRasterXSize(hSrcDs);
    const int height = GDALGetRasterYSize(hSrcDs);
    const int bandCount = GDALGetRasterCount(hSrcDs);

    const std::string formulaStr = formula ? std::string(formula) : "";
    const std::string bandsStr = bands ? std::string(bands) : "";
    const std::string presetStr = preset ? std::string(preset) : "";
    const std::string filterStr = bandFilter ? std::string(bandFilter) : "";
    const std::string colormapStr = colormap ? std::string(colormap) : "";
    const std::string rescaleStr = rescale ? std::string(rescale) : "";

    // Get source geotransform and projection
    double geoTransform[6];
    bool hasGeoTransform = (GDALGetGeoTransform(hSrcDs, geoTransform) == CE_None);
    const char* projRef = GDALGetProjectionRef(hSrcDs);
    std::string projection = projRef ? std::string(projRef) : "";

    if (!formulaStr.empty()) {
        // Formula mode: apply formula + colormap, export as RGBA GeoTIFF
        // TODO: For very large rasters, consider processing in blocks/scanlines
        // to avoid excessive memory usage (currently loads all bands fully into memory)
        auto& ve = ddb::VegetationEngine::instance();

        ddb::BandFilter bf;
        if (!filterStr.empty()) {
            bf = ddb::VegetationEngine::parseFilter(filterStr, bandCount);
        } else {
            bf = ve.autoDetectFilter(std::string(inputPath));
        }

        size_t pixCount = static_cast<size_t>(width) * height;
        std::vector<std::vector<float>> bandDataStorage(bandCount);
        std::vector<float*> bandDataPtrs(bandCount);

        // Read bands and detect alpha/nodata
        auto nodataInfo = detectBandNodata(hSrcDs, bandCount);
        for (int b = 0; b < bandCount; b++) {
            bandDataStorage[b].resize(pixCount);
            bandDataPtrs[b] = bandDataStorage[b].data();
            GDALRasterBandH hBand = GDALGetRasterBand(hSrcDs, b + 1);
            if (GDALRasterIO(hBand, GF_Read, 0, 0, width, height,
                             bandDataPtrs[b], width, height,
                             GDT_Float32, 0, 0) != CE_None) {
                GDALClose(hSrcDs);
                throw GDALException("Cannot read band " + std::to_string(b + 1));
            }
        }

        // Pre-mask transparent and nodata pixels
        float nodata = NODATA_SENTINEL;
        premaskNodata(bandDataPtrs, pixCount, bandCount, nodataInfo, nodata);

        // Apply formula
        std::vector<float> result(pixCount);
        const auto* formulaPtr = ve.getFormula(formulaStr);
        if (!formulaPtr) {
            GDALClose(hSrcDs);
            throw InvalidArgsException("Unknown formula: " + formulaStr);
        }
        ve.applyFormula(*formulaPtr, bf, bandDataPtrs, result.data(), pixCount, nodata);

        // Determine rescale range
        float rMin, rMax;
        if (!rescaleStr.empty()) {
            auto commaPos = rescaleStr.find(',');
            if (commaPos == std::string::npos) {
                GDALClose(hSrcDs);
                throw InvalidArgsException("Invalid rescale format");
            }
            rMin = std::stof(rescaleStr.substr(0, commaPos));
            rMax = std::stof(rescaleStr.substr(commaPos + 1));
        } else if (formulaPtr->hasRange && formulaPtr->rangeMin != formulaPtr->rangeMax) {
            rMin = static_cast<float>(formulaPtr->rangeMin);
            rMax = static_cast<float>(formulaPtr->rangeMax);
        } else {
            std::vector<float> valid;
            valid.reserve(pixCount);
            for (size_t i = 0; i < pixCount; i++) {
                if (result[i] != nodata) valid.push_back(result[i]);
            }
            if (!valid.empty()) {
                std::sort(valid.begin(), valid.end());
                rMin = valid[static_cast<size_t>(valid.size() * 0.02)];
                rMax = valid[std::min(valid.size() - 1, static_cast<size_t>(valid.size() * 0.98))];
            } else {
                rMin = 0; rMax = 1;
            }
        }

        // Apply colormap
        std::string cmId = colormapStr.empty() ? "rdylgn" : colormapStr;
        const auto* cmap = ve.getColormap(cmId);
        if (!cmap) {
            GDALClose(hSrcDs);
            throw InvalidArgsException("Unknown colormap: " + cmId);
        }
        std::vector<uint8_t> rgba(pixCount * 4);
        ve.applyColormap(result.data(), rgba.data(), pixCount, *cmap, rMin, rMax, nodata);

        GDALClose(hSrcDs);

        // Create output GeoTIFF with 4 bands (RGBA)
        GDALDriverH gtiffDrv = GDALGetDriverByName("GTiff");
        if (!gtiffDrv)
            throw GDALException("GTiff driver not available");

        char** createOpts = nullptr;
        createOpts = CSLAddString(createOpts, "COMPRESS=DEFLATE");

        GDALDatasetH hOut = GDALCreate(gtiffDrv, outputPath, width, height, 4, GDT_Byte, createOpts);
        CSLDestroy(createOpts);
        if (!hOut)
            throw GDALException("Cannot create output GeoTIFF");

        if (hasGeoTransform) GDALSetGeoTransform(hOut, geoTransform);
        if (!projection.empty()) GDALSetProjection(hOut, projection.c_str());

        for (int b = 0; b < 4; b++) {
            std::vector<uint8_t> chanData(pixCount);
            for (size_t i = 0; i < pixCount; i++) chanData[i] = rgba[i * 4 + b];
            GDALRasterBandH hBand = GDALGetRasterBand(hOut, b + 1);
            GDALRasterIO(hBand, GF_Write, 0, 0, width, height,
                         chanData.data(), width, height, GDT_Byte, 0, 0);
        }
        GDALSetRasterColorInterpretation(GDALGetRasterBand(hOut, 1), GCI_RedBand);
        GDALSetRasterColorInterpretation(GDALGetRasterBand(hOut, 2), GCI_GreenBand);
        GDALSetRasterColorInterpretation(GDALGetRasterBand(hOut, 3), GCI_BlueBand);
        GDALSetRasterColorInterpretation(GDALGetRasterBand(hOut, 4), GCI_AlphaBand);

        GDALFlushCache(hOut);
        GDALClose(hOut);
    } else {
        // Non-formula mode: band selection + rescale, export as GeoTIFF
        std::vector<int> selectedBands;
        if (!bandsStr.empty()) {
            std::istringstream ss(bandsStr);
            std::string token;
            while (std::getline(ss, token, ',')) {
                int b = std::stoi(token);
                if (b < 1 || b > bandCount)
                    throw InvalidArgsException("Band index out of range: " + token);
                selectedBands.push_back(b);
            }
        } else if (!presetStr.empty()) {
            auto& spm = ddb::SensorProfileManager::instance();
            auto mapping = spm.getBandMappingForPreset(std::string(inputPath), presetStr);
            selectedBands = {mapping.r, mapping.g, mapping.b};
        } else {
            for (int i = 1; i <= std::min(3, bandCount); i++) selectedBands.push_back(i);
        }

        // Use GDALTranslate for band selection + rescale
        char** targs = nullptr;
        targs = CSLAddString(targs, "-ot");
        targs = CSLAddString(targs, "Byte");

        for (int b : selectedBands) {
            targs = CSLAddString(targs, "-b");
            targs = CSLAddString(targs, std::to_string(b).c_str());
        }

        GDALDataType srcType = GDALGetRasterDataType(GDALGetRasterBand(hSrcDs, 1));
        if (srcType != GDT_Byte) {
            if (!rescaleStr.empty()) {
                auto commaPos = rescaleStr.find(',');
                if (commaPos != std::string::npos) {
                    targs = CSLAddString(targs, "-scale");
                    targs = CSLAddString(targs, rescaleStr.substr(0, commaPos).c_str());
                    targs = CSLAddString(targs, rescaleStr.substr(commaPos + 1).c_str());
                    targs = CSLAddString(targs, "0");
                    targs = CSLAddString(targs, "255");
                }
            } else {
                targs = CSLAddString(targs, "-scale");
            }
        }

        targs = CSLAddString(targs, "-of");
        targs = CSLAddString(targs, "GTiff");
        targs = CSLAddString(targs, "-co");
        targs = CSLAddString(targs, "COMPRESS=DEFLATE");

        auto psOptions = GDALTranslateOptionsNew(targs, nullptr);
        CSLDestroy(targs);
        if (!psOptions) {
            GDALClose(hSrcDs);
            throw GDALException("Cannot create GDALTranslate options");
        }

        int bUsageError = FALSE;
        GDALDatasetH hOut = GDALTranslate(outputPath, hSrcDs, psOptions, &bUsageError);
        GDALTranslateOptionsFree(psOptions);
        GDALClose(hSrcDs);

        if (!hOut || bUsageError)
            throw GDALException("Cannot create output GeoTIFF");

        GDALFlushCache(hOut);
        GDALClose(hOut);
    }

    DDB_C_END
}

DDB_DLL DDBErr DDBGetThermalInfo(const char *filePath, char **output) {
    DDB_C_BEGIN
    if (utils::isNullOrEmptyOrWhitespace(filePath))
        throw InvalidArgsException("No file path provided");
    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    std::string jsonStr = ddb::getThermalInfoJson(std::string(filePath));
    utils::copyToPtr(jsonStr, output);
    DDB_C_END
}

DDB_DLL DDBErr DDBGetThermalPoint(const char *filePath, int x, int y, char **output) {
    DDB_C_BEGIN
    if (utils::isNullOrEmptyOrWhitespace(filePath))
        throw InvalidArgsException("No file path provided");
    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    std::string jsonStr = ddb::getThermalPointJson(std::string(filePath), x, y);
    utils::copyToPtr(jsonStr, output);
    DDB_C_END
}

DDB_DLL DDBErr DDBGetThermalAreaStats(const char *filePath, int x0, int y0, int x1, int y1, char **output) {
    DDB_C_BEGIN
    if (utils::isNullOrEmptyOrWhitespace(filePath))
        throw InvalidArgsException("No file path provided");
    if (output == nullptr)
        throw InvalidArgsException("Output pointer is null");

    std::string jsonStr = ddb::getThermalAreaStatsJson(std::string(filePath), x0, y0, x1, y1);
    utils::copyToPtr(jsonStr, output);
    DDB_C_END
}
