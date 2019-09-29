/* Copyright 2019 MasseranoLabs LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#ifndef UTILS_H
#define UTILS_H

#include <algorithm>
#include <cctype>
#include <string>
#include <filesystem>
#include <sys/types.h>
#include <sys/stat.h>
#include "classes/exceptions.h"
#include "logger.h"

#ifdef WIN32
#include <windows.h>    //GetModuleFileNameW
#define stat _stat
#else
#include <limits.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#endif

namespace fs = std::filesystem;

namespace utils {

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

// Compares an extension with a list of extension strings
// @return true if the extension matches one of those in the list
bool checkExtension(const fs::path &extension, const std::initializer_list<std::string>& matches);

time_t getModifiedTime(const std::string &filePath);
off_t getSize(const std::string &filePath);
bool pathsAreChildren(const fs::path &parentPath, const std::vector<std::string> &childPaths);
int pathDepth(const fs::path &path);

fs::path getExeFolderPath();

}

#endif // UTILS_H
