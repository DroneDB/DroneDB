/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef UTILS_H
#define UTILS_H

#include <memory>
#include <iostream>
#include <sstream>
#include <cstdio>
#include <cctype>
#include <string>
#include <cmath>
#include <sys/types.h>
#include <sys/stat.h>
#include "exceptions.h"
#include "logger.h"
#include "fs.h"

#ifndef M_PI
    #define M_PI 3.1415926535
#endif

#define F_EPSILON 0.000001

#ifdef WIN32
    
    // Avoid defining min / max macros
    #define NOMINMAX

    #include <windows.h>    //GetModuleFileNameW
    #include <direct.h> // _getcwd
#define stat _stat
#else
    #include <limits.h>
    #include <termios.h>
    #include <unistd.h>
#endif

#ifndef PATH_MAX
    #define PATH_MAX 4096
#endif

namespace utils {

using namespace ddb;

static inline void toLower(std::string &s) {
    std::transform(s.begin(), s.end(), s.begin(),[](int ch) {
        return std::tolower(ch);
    });
}

static inline void toUpper(std::string &s) {
    std::transform(s.begin(), s.end(), s.begin(),[](int ch) {
        return std::toupper(ch);
    });
}

static inline void ltrim(std::string &s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](int ch) {
        return !std::isspace(ch);
    }));
}

static inline void rtrim(std::string &s) {
    s.erase(std::find_if(s.rbegin(), s.rend(), [](int ch) {
        return !std::isspace(ch);
    }).base(), s.end());
}

static inline void trim(std::string &s) {
    ltrim(s);
    rtrim(s);
}

static inline double rad2deg(double rad) {
    return (rad * 180.0) / M_PI;
}

static inline double deg2rad(double deg) {
    return (deg * M_PI) / 180.0;
}

static inline bool sameFloat(float a, float b){
    return fabs(a - b) < F_EPSILON;
}

// https://stackoverflow.com/questions/2342162/stdstring-formatting-like-sprintf/25440014
template<typename ... Args>
std::string stringFormat( const std::string& format, Args ... args ) {
    size_t size = snprintf( nullptr, 0, format.c_str(), args ... ) + 1; // Extra space for '\0'
    std::unique_ptr<char[]> buf( new char[ size ] );
    snprintf( buf.get(), size, format.c_str(), args ... );
    return std::string( buf.get(), buf.get() + size - 1 ); // We don't want the '\0' inside
}

//https://stackoverflow.com/questions/16605967/set-precision-of-stdto-string-when-converting-floating-point-values
template <typename T>
std::string to_str(const T value, const int n = 6)
{
    std::ostringstream out;
    out.precision(n);
    out << std::fixed << value;
    return out.str();
}

std::string getPrompt(const std::string &prompt = "");

// Cross-platform getpass
std::string getPass(const std::string &prompt = "Password: ");

}

// Fix for removing macros
#undef max
#undef min

#endif // UTILS_H
