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
#include <experimental/filesystem>

namespace fs = std::experimental::filesystem;

namespace utils{

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
static bool checkExtension(const fs::path &extension, const std::initializer_list<std::string>& matches) {
    std::string ext = extension.string();
    if (ext.size() < 1) return false;
    std::string extLowerCase = ext.substr(1, ext.size());
    utils::toLower(extLowerCase);

    for (auto &m : matches) {
        if (m == extLowerCase) return true;
    }
    return false;
}

}

#endif // UTILS_H
