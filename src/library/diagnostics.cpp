/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "diagnostics.h"

#include <curl/curl.h>
#include <gdal_inc.h>
#include <sqlite3.h>
#include <spatialite.h>
#include <proj.h>

#include <clocale>
#include <cstdlib>
#include <pdal/pdal_features.hpp>
#include <sstream>

#include "logger.h"
#include "version.h"

namespace ddb::diagnostics {

std::string getBuildInfo() {
    std::ostringstream ss;

#ifndef NDEBUG
    ss << "Debug";
#else
    ss << "Release";
#endif

    ss << " build";

#ifdef __GNUC__
    ss << " (GCC " << __GNUC__ << "." << __GNUC_MINOR__ << ")";
#elif defined(_MSC_VER)
    ss << " (MSVC " << _MSC_VER << ")";
#endif

    return ss.str();
}

std::string getEnvOrUnset(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string("<unset>");
}

void printVersions() {

    LOGV << "DDB v" << ddb::version();
    LOGD << "Build info: " << getBuildInfo();

    LOGV << "SQLite: " << sqlite3_libversion();
    LOGV << "SpatiaLite: " << spatialite_version();
    LOGV << "GDAL: " << GDALVersionInfo("RELEASE_NAME");
    LOGV << "CURL: " << curl_version();
    LOGV << "PDAL: " << pdal::pdalVersion;

    LOGV << "PROJ: " << proj_info().release;

    // The data folders libddb really uses: GDAL's config option (which falls back to the
    // environment) and the search paths of the PROJ contexts GDAL creates (PROJ_DATA/PROJ_LIB of
    // the host, or what the process initialization gave them), not the environment variables,
    // which libddb no longer sets
    const char* gdalData = CPLGetConfigOption("GDAL_DATA", nullptr);
    LOGV << "GDAL_DATA = " << (gdalData != nullptr && *gdalData != '\0' ? gdalData : "<unset>");
    CPLStringList projPaths(OSRGetPROJSearchPaths());
    std::string projSearchPaths;
    for (int i = 0; i < projPaths.Count(); i++)
        projSearchPaths += (i > 0 ? std::string(";") : std::string()) + projPaths[i];
    LOGV << "PROJ search paths = " << (projSearchPaths.empty() ? "<unset>" : projSearchPaths);

    LOGD << "Current locale (LC_ALL): " << std::setlocale(LC_ALL, nullptr);
    LOGD << "Current locale (LC_CTYPE): " << std::setlocale(LC_CTYPE, nullptr);
    LOGD << "LC_ALL env var: " << getEnvOrUnset("LC_ALL");

}

}  // namespace ddb::diagnostics
