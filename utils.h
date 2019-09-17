#ifndef UTILS_H
#define UTILS_H

#include <algorithm>
#include <cctype>
#include <string>

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

}

#endif // UTILS_H
