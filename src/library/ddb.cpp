/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "ddb.h"

#include <cpr/cpr.h>

#include <csignal>
#include <mutex>
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
#include "mio.h"
#include "passwordmanager.h"
#include "stac.h"
#include "status.h"
#include "syncmanager.h"
#include "tagmanager.h"
#include "thumbs.h"
#include "tilerhelper.h"
#include "utils.h"
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
    std::vector<EntryType> typeFilter;
    if (types != nullptr && strlen(types) > 0)
    {
        std::stringstream ss(types);
        std::string item;
        while (std::getline(ss, item, ','))
        {
            // Trim whitespace
            size_t start = item.find_first_not_of(" \t");
            size_t end = item.find_last_not_of(" \t");
            if (start != std::string::npos && end != std::string::npos)
                item = item.substr(start, end - start + 1);

            // Convert string to EntryType
            EntryType t = typeFromHuman(item);
            if (t == EntryType::Undefined)
                throw InvalidArgsException("Unknown entry type: " + item);
            if (t == EntryType::Directory)
                throw InvalidArgsException("Cannot rescan directories");
            typeFilter.push_back(t);
        }
    }

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
