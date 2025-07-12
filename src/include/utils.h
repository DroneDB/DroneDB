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
#include "exceptions.h"
#include "fs.h"
#include "logger.h"

#ifndef M_PI
#define M_PI 3.1415926535
#endif

#define F_EPSILON 0.000001

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

static inline void toLower(std::string& s) {
    std::transform(s.begin(), s.end(), s.begin(), [](int ch) { return std::tolower(ch); });
}

static inline void toUpper(std::string& s) {
    std::transform(s.begin(), s.end(), s.begin(), [](int ch) { return std::toupper(ch); });
}

static inline void ltrim(std::string& s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](int ch) { return !std::isspace(ch); }));
}

static inline void rtrim(std::string& s) {
    s.erase(std::find_if(s.rbegin(), s.rend(), [](int ch) { return !std::isspace(ch); }).base(),
            s.end());
}

static inline void trim(std::string& s) {
    ltrim(s);
    rtrim(s);
}

static inline double rad2deg(double rad) {
    return (rad * 180.0) / M_PI;
}

static inline double deg2rad(double deg) {
    return (deg * M_PI) / 180.0;
}

static inline bool sameFloat(float a, float b) {
    return fabs(a - b) < F_EPSILON;
}

static inline std::vector<std::string> split(const std::string& s, const std::string& delimiter) {
    size_t posStart = 0, posEnd, delimLen = delimiter.length();
    std::string token;
    std::vector<std::string> res;

    while ((posEnd = s.find(delimiter, posStart)) != std::string::npos) {
        token = s.substr(posStart, posEnd - posStart);
        posStart = posEnd + delimLen;
        res.push_back(token);
    }

    res.push_back(s.substr(posStart));
    return res;
}

// TODO: To replace with std::format when we move to C++20
// https://stackoverflow.com/questions/2342162/stdstring-formatting-like-sprintf/25440014
template <typename... Args>
std::string stringFormat(const std::string& format, Args... args) {
    const size_t size = snprintf(nullptr, 0, format.c_str(), args...) + 1;  // Extra space for '\0'
    if (size <= 0) {
        throw std::runtime_error("Error during formatting.");
    }
    const std::unique_ptr<char[]> buf(new char[size]);
    snprintf(buf.get(), size, format.c_str(), args...);
    return std::string(buf.get(), buf.get() + size - 1);  // We don't want the '\0' inside
}

// https://stackoverflow.com/questions/16605967/set-precision-of-stdto-string-when-converting-floating-point-values
template <typename T>
std::string toStr(const T value, const int n = 6) {
    std::ostringstream out;
    out.precision(n);
    out << std::fixed << value;
    return out.str();
}

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

// https://stackoverflow.com/questions/3418231/replace-part-of-a-string-with-another-string
void stringReplace(std::string& str, const std::string& from, const std::string& to);

void sleep(int msecs);

DDB_DLL std::string generateRandomString(int length);
std::string join(const std::vector<std::string>& vec, char separator = ',');

DDB_DLL bool hasDotNotation(const std::string& path);

DDB_DLL bool isLowerCase(const std::string& str);

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

DDB_DLL void printVersions();

DDB_DLL bool isNullOrEmptyOrWhitespace(const char* str, size_t maxLength = 0);
DDB_DLL bool isNullOrEmptyOrWhitespace(const char** strlist, int count, size_t maxLength = 0);

// Validate that no string in an array is null
DDB_DLL bool hasNullStringInArray(const char** strlist, int count);

DDB_DLL bool isValidArrayParam(const char** array, int count);

// Validate string parameter allowing empty but not null
DDB_DLL bool isValidStringParam(const char* str);

// Validate string parameter requiring non-empty
DDB_DLL bool isValidNonEmptyStringParam(const char* str);

// Fix for removing macros
#undef max
#undef min

}  // namespace utils
}  // namespace ddb

#endif  // UTILS_H
