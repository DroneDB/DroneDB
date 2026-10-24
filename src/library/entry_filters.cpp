/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "entry_filters.h"

#include <algorithm>
#include <cstring>

#include "exceptions.h"
#include "stringutil.h"
#include "utf8.h"

namespace ddb::filters {

std::vector<EntryType> parseEntryTypeList(const char* types, bool allowDirectory) {
    std::vector<EntryType> typeFilter;

    if (types == nullptr || std::strlen(types) == 0)
        return typeFilter;

    // "image," is the list "image": a trailing separator ends the list, as before DQ-05
    std::vector<std::string> pieces = str::split(types, ',');
    if (pieces.back().empty())
        pieces.pop_back();

    for (const std::string& piece : pieces) {
        const std::string item = str::trimCopy(piece);

        // Convert string to EntryType
        const EntryType t = typeFromHuman(item);
        if (t == EntryType::Undefined)
            throw InvalidArgsException("Unknown entry type: " + item);
        if (t == EntryType::Directory && !allowDirectory)
            throw InvalidArgsException("Cannot rescan directories");
        typeFilter.push_back(t);
    }

    return typeFilter;
}

namespace {

// Greedy wildcard match: '*' matches any characters (also none), '?' one. `next(text, n)` is
// the index after the character at text[n], `same(p, c)` whether the pattern character p
// matches the text character c. On a mismatch the last '*' takes one more character.
template <typename Text, typename Next, typename Same>
bool wildcardMatch(const Text& name, const Text& pattern, Next next, Same same) {
    size_t n = 0, p = 0;
    size_t starP = std::string::npos, starN = 0;
    while (n < name.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            starP = p++;
            starN = n;
        } else if (p < pattern.size() && pattern[p] == '?') {
            p++;
            n = next(name, n);
        } else if (p < pattern.size() && same(pattern[p], name[n])) {
            p++;
            n++;
        } else if (starP != std::string::npos) {
            p = starP + 1;
            n = ++starN;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*')
        p++;
    return p == pattern.size();
}

bool isAscii(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](char c) {
        return static_cast<unsigned char>(c) < 0x80;
    });
}

}  // namespace

// XI-09: the comparison folds case for every script (Unicode simple case folding, utf8.h) when
// both strings are valid UTF-8; libddb 1.12 and earlier folded ASCII letters only. Otherwise
// (and for ASCII-only strings, where both rules agree) it compares bytes, ASCII letters in any
// case, and '?' takes a UTF-8 lead byte with its continuation bytes.
bool matchesNamePattern(const std::string& name, const std::string& pattern) {
    std::u32string foldedName, foldedPattern;
    if ((!isAscii(name) || !isAscii(pattern)) && utf8::foldedCodePoints(name, foldedName) &&
        utf8::foldedCodePoints(pattern, foldedPattern))
        return wildcardMatch(
            foldedName,
            foldedPattern,
            [](const std::u32string&, size_t n) { return n + 1; },
            [](char32_t a, char32_t b) { return a == b; });

    return wildcardMatch(
        name,
        pattern,
        [](const std::string& text, size_t n) {
            n++;
            while (n < text.size() && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80)
                n++;
            return n;
        },
        [](char a, char b) { return str::toLowerChar(a) == str::toLowerChar(b); });
}

std::vector<std::string> parseExtensionList(const char* extensions) {
    std::vector<std::string> result;

    if (extensions == nullptr || std::strlen(extensions) == 0)
        return result;

    for (const std::string& piece : str::split(extensions, ',')) {
        const std::string item = str::trimCopy(piece);
        if (item.empty())
            continue;

        std::string ext = item[0] == '.' ? item.substr(1) : item;
        if (ext.empty() || ext.find_first_of("/\\*? \t") != std::string::npos)
            throw InvalidArgsException("Invalid extension: " + item);

        str::toLower(ext);
        result.push_back(ext);
    }

    return result;
}

bool hasAnyExtension(const std::string& path, const std::vector<std::string>& extensions) {
    for (const auto& ext : extensions) {
        // "a.dwg" matches "dwg", "x.copc.laz" matches "copc.laz" and "laz", "dwg" alone does not
        if (path.size() > ext.size() + 1 && str::endsWithCI(path, ext) &&
            path[path.size() - ext.size() - 1] == '.')
            return true;
    }

    return false;
}

}  // namespace ddb::filters
