/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef STRINGUTIL_H
#define STRINGUTIL_H

// The string helpers of libddb (DQ-05), ddb::str: the one home of case conversion, trimming,
// splitting and joining. Nothing here depends on the locale: letters are the ASCII ones, and
// every byte of a multi-byte UTF-8 sequence (high bit set) is left as it is. For comparisons
// that fold case for every script see utf8.h (utf8::foldCase).
//
// Not named strings.h: with this directory on the include path, <string.h> of glibc would
// include it instead of the POSIX <strings.h>.

#include <cctype>
#include <string>
#include <string_view>
#include <vector>


namespace ddb::str {

// ------------------------------------------------------------------ characters
//
// <cctype> functions take an int that must be EOF or representable as unsigned char: a plain
// (signed) char with the high bit set (any UTF-8 lead or continuation byte) is undefined
// behaviour. These wrappers do the unsigned char conversion once, and are the only place in
// libddb that calls <cctype>.

inline char toLowerChar(char c) noexcept {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

inline char toUpperChar(char c) noexcept {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
}

inline bool isDigitChar(char c) noexcept {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

inline bool isAlphaChar(char c) noexcept {
    return std::isalpha(static_cast<unsigned char>(c)) != 0;
}

inline bool isAlnumChar(char c) noexcept {
    return std::isalnum(static_cast<unsigned char>(c)) != 0;
}

inline bool isUpperChar(char c) noexcept {
    return std::isupper(static_cast<unsigned char>(c)) != 0;
}

// ' ', \t, \n, \v, \f, \r
inline bool isSpaceChar(char c) noexcept {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

// ------------------------------------------------------------------ case

inline void toLower(std::string& s) noexcept {
    for (char& c : s)
        c = toLowerChar(c);
}

inline void toUpper(std::string& s) noexcept {
    for (char& c : s)
        c = toUpperChar(c);
}

inline std::string toLowerCopy(std::string_view s) {
    std::string result(s);
    toLower(result);
    return result;
}

// True when s has no uppercase letter (ASCII)
inline bool isLowerCase(std::string_view s) noexcept {
    for (char c : s)
        if (isUpperChar(c))
            return false;
    return true;
}

// Equality, ASCII letters in any case
inline bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++)
        if (toLowerChar(a[i]) != toLowerChar(b[i]))
            return false;
    return true;
}

// starts_with / ends_with, ASCII letters in any case
inline bool startsWithCI(std::string_view s, std::string_view prefix) noexcept {
    return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

inline bool endsWithCI(std::string_view s, std::string_view suffix) noexcept {
    return s.size() >= suffix.size() && iequals(s.substr(s.size() - suffix.size()), suffix);
}

// ------------------------------------------------------------------ trimming

// The characters isSpaceChar accepts
inline constexpr std::string_view kWhitespace = " \t\n\v\f\r";

// s without the leading and trailing characters of `chars` (a view into s)
inline std::string_view trimView(std::string_view s, std::string_view chars = kWhitespace) noexcept {
    const size_t first = s.find_first_not_of(chars);
    if (first == std::string_view::npos)
        return {};
    return s.substr(first, s.find_last_not_of(chars) - first + 1);
}

inline std::string trimCopy(std::string_view s, std::string_view chars = kWhitespace) {
    return std::string(trimView(s, chars));
}

inline void trim(std::string& s) {
    s = trimCopy(s);
}

// ------------------------------------------------------------------ split, join, replace

// The pieces of s between the occurrences of delimiter, empty pieces included ("a,,b" gives
// "a", "", "b"; "" gives one empty piece). An empty delimiter gives s as the only piece.
std::vector<std::string> split(std::string_view s, std::string_view delimiter);
std::vector<std::string> split(std::string_view s, char delimiter);

// The runs of non-whitespace characters (isSpaceChar) of s, in order; none for blank text
std::vector<std::string> splitWhitespace(std::string_view s);

// The items with separator between them (no trailing separator; "" for no items)
std::string join(const std::vector<std::string>& items, std::string_view separator = ",");

// s with every occurrence of `from` replaced by `to`, left to right, without rescanning the
// replacement; an empty `from` changes nothing
void replaceAll(std::string& s, std::string_view from, std::string_view to);

}  // namespace ddb::str

#endif  // STRINGUTIL_H
