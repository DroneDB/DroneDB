/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef UTILS_H
#define UTILS_H

#include <cpr/cpr.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include "ddb_export.h"
#include "entry_types.h"
#include "exceptions.h"
#include "fs.h"
#include "logger.h"
#include "mathutil.h"

#ifdef WIN32
#include <direct.h>   // _getcwd
#include <windows.h>  //GetModuleFileNameW
#else
#include <limits.h>
#include <termios.h>
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

namespace ddb {
namespace utils {

// Allocates memory to copy str into **ptr        // The caller is responsible for deallocating
// the pointer
static inline void copyToPtr(const std::string& str, char** ptr) {
    if (ptr != NULL) {
        size_t s = str.size();
        *ptr = (char*)calloc(s + 1, sizeof(char));
        if (*ptr == nullptr)
            throw std::bad_alloc();

        strcpy(*ptr, str.c_str());
    }
}

DDB_DLL std::string getPrompt(const std::string& prompt = "");

// Cross-platform getpass
DDB_DLL std::string getPass(const std::string& prompt = "Password: ");

time_t currentUnixTimestamp();

void sleep(int msecs);

DDB_DLL std::string generateRandomString(int length);

DDB_DLL bool hasDotNotation(const std::string& path);

DDB_DLL bool isNetworkPath(const std::string& path);

cpr::Response downloadToFile(const std::string& url,
                             const std::string& filePath,
                             bool throwOnError = false,
                             bool verifySsl = true);

std::string readFile(const std::string& url, bool throwOnError = false, bool verifySsl = true);

typedef std::function<bool(std::string& fileName, size_t txBytes, size_t totalBytes)>
    UploadCallback;

cpr::Header authHeader(const std::string& token);

cpr::Header authCookie(const std::string& token);

// A file part of a multipart upload. cpr would name the part with fs::path(std::string), the
// ANSI code page on Windows: the UTF-8 file name is passed instead (mio.h, "Path encoding")
cpr::File uploadFile(const fs::path& file);

DDB_DLL void printVersions();

// "Debug build ..." or "Release build ..." (from NDEBUG), compiler and build date
DDB_DLL std::string getBuildInfo();

// The value of an environment variable, or "<unset>" when it is not set (std::getenv
// returns null, which must never be streamed)
DDB_DLL std::string getEnvOrUnset(const char* name);

DDB_DLL bool isNullOrEmptyOrWhitespace(const char* str, size_t maxLength = 0);
DDB_DLL bool isNullOrEmptyOrWhitespace(const char** strlist, int count, size_t maxLength = 0);

// Validate that no string in an array is null
DDB_DLL bool hasNullStringInArray(const char** strlist, int count);

DDB_DLL bool isValidArrayParam(const char** array, int count);

// Validate string parameter allowing empty but not null
DDB_DLL bool isValidStringParam(const char* str);

// Validate string parameter requiring non-empty
DDB_DLL bool isValidNonEmptyStringParam(const char* str);

// Parse a comma-separated list of entry type names into a vector of EntryType
// Returns an empty vector if types is null or empty
// Throws InvalidArgsException if an unknown type is found, or if Directory is specified and
// allowDirectory is false (rescans skip directories)
DDB_DLL std::vector<ddb::EntryType> parseEntryTypeList(const char* types, bool allowDirectory = false);

// True if name matches pattern, where '*' matches any characters (also none) and '?' one
// (UTF-8) character. Letters match regardless of case: Unicode simple case folding
// (utf8::foldCase) when both are valid UTF-8, else a byte comparison with ASCII letters in any
// case.
DDB_DLL bool matchesNamePattern(const std::string& name, const std::string& pattern);

// Parse a comma-separated list of file extensions ("dwg,.DXF,copc.laz") into
// lowercase extensions without the leading dot. Empty items are skipped.
// Returns an empty vector if extensions is null or empty
// Throws InvalidArgsException if an item is a bare dot or contains a path separator,
// a wildcard or whitespace
DDB_DLL std::vector<std::string> parseExtensionList(const char* extensions);

// True if path ends with "." + one of extensions (lowercase, without the dot, as
// returned by parseExtensionList). Comparison ignores case
DDB_DLL bool hasAnyExtension(const std::string& path, const std::vector<std::string>& extensions);

// Fix for removing macros
#undef max
#undef min

}  // namespace utils
}  // namespace ddb

#endif  // UTILS_H
