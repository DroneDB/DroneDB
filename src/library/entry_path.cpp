/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "entry_path.h"

#include <algorithm>

#include "ddb.h"
#include "exceptions.h"
#include "utf8.h"

namespace ddb {

namespace {

// The rejection codes (entry_path.h): stable strings, part of the DDBNormalizePath contract
constexpr std::string_view kEmpty = "empty";
constexpr std::string_view kNul = "nul";
constexpr std::string_view kEncoding = "encoding";
constexpr std::string_view kAbsolute = "absolute";
constexpr std::string_view kTraversal = "traversal";
constexpr std::string_view kReserved = "reserved";

bool isAsciiLetter(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// A POSIX or Windows root, a UNC or device path, or a Windows drive ("C:\x", "C:x")
bool isRooted(std::string_view path) noexcept {
    if (!path.empty() && isEntryPathSeparator(path.front()))
        return true;
    return path.size() >= 2 && isAsciiLetter(path[0]) && path[1] == ':';
}

// The segments of path between separators, without the empty and "." ones
std::vector<std::string_view> segmentsOf(std::string_view path) {
    std::vector<std::string_view> segments;
    size_t start = 0;
    while (start <= path.size()) {
        const auto end =
            std::find_if(path.begin() + start, path.end(), isEntryPathSeparator) - path.begin();
        const auto segment = path.substr(start, end - start);
        if (!segment.empty() && segment != ".")
            segments.push_back(segment);
        start = end + 1;
    }
    return segments;
}

EntryPathResult rejection(std::string_view code) {
    return {"", code};
}

}  // namespace

ReservedRules ReservedRules::fromJson(const char* text) {
    ReservedRules rules;
    if (text == nullptr || *text == '\0')
        return rules;

    json names;
    try {
        names = json::parse(text);
    } catch (const json::exception& e) {
        throw InvalidArgsException(std::string("Invalid reserved names (a JSON array of names): ") +
                                   e.what());
    }
    if (!names.is_array())
        throw InvalidArgsException("Invalid reserved names: expected a JSON array of names");

    for (const auto& name : names) {
        if (!name.is_string())
            throw InvalidArgsException("Invalid reserved name: " + name.dump() +
                                       " (expected a string)");
        const auto value = name.get<std::string>();
        // One whole segment, as normalizeEntryPath splits it
        if (segmentsOf(value) != std::vector<std::string_view>{value} || value == ".." ||
            value.find('\0') != std::string::npos)
            throw InvalidArgsException("Invalid reserved name: " + name.dump() +
                                       " (expected one path segment)");
        rules.firstSegment.push_back(value);
    }
    return rules;
}

json EntryPathResult::toJson() const {
    if (accepted())
        return json{{"path", path}};
    return json{{"rejected", std::string(rejected)}};
}

bool isEntryPathSeparator(char c) noexcept {
    return c == '/' || c == '\\';
}

bool isReservedSegment(std::string_view segment) noexcept {
    return segment == DDB_FOLDER;
}

bool isReservedSegment(const fs::path& segment) {
    static const fs::path reserved(DDB_FOLDER);
    return segment == reserved;
}

EntryPathResult normalizeEntryPath(std::string_view path, const ReservedRules& rules) {
    if (path.empty())
        return rejection(kEmpty);
    if (path.find('\0') != std::string_view::npos)
        return rejection(kNul);
    if (!utf8::isValid(path))
        return rejection(kEncoding);
    if (isRooted(path))
        return rejection(kAbsolute);

    const auto segments = segmentsOf(path);
    if (std::find(segments.begin(), segments.end(), "..") != segments.end())
        return rejection(kTraversal);
    if (segments.empty())
        return rejection(kEmpty);

    const bool reserved =
        std::any_of(segments.begin(),
                    segments.end(),
                    [](std::string_view s) { return isReservedSegment(s); }) ||
        std::find(rules.firstSegment.begin(), rules.firstSegment.end(), segments.front()) !=
            rules.firstSegment.end();
    if (reserved)
        return rejection(kReserved);

    EntryPathResult result;
    for (const auto segment : segments) {
        if (!result.path.empty())
            result.path += '/';
        result.path += segment;
    }
    return result;
}

}  // namespace ddb
