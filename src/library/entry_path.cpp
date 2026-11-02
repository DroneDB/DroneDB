/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "entry_path.h"
#include "json.h"

#include <algorithm>

#include "ddb.h"
#include "exceptions.h"
#include "mio.h"
#include "stringutil.h"
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

void to_json(json& j, const EntryPathResult& result) {
    if (result.accepted())
        j = json{{"path", result.path}};
    else
        j = json{{"rejected", std::string(result.rejected)}};
}

bool isEntryPathSeparator(char c) noexcept {
    return c == '/' || c == '\\';
}

bool isReservedSegment(std::string_view segment) noexcept {
    // Windows opens "x::$INDEX_ALLOCATION" (an NTFS stream of the folder x) as the folder x
    if (const auto stream = segment.find(':'); stream != std::string_view::npos)
        segment = segment.substr(0, stream);
    // Win32 drops trailing dots and spaces from a name: ".ddb." and ".ddb " open ".ddb"
    while (!segment.empty() && (segment.back() == '.' || segment.back() == ' '))
        segment.remove_suffix(1);
    // A case-insensitive file system (Windows, macOS) opens ".DDB" as ".ddb"
    if (str::iequals(segment, DDB_FOLDER))
        return true;
    // NTFS gives ".ddb" an 8.3 short name, "DDB~1" (or ~2, ... when that one is taken), which
    // opens the folder as well. Any number counts: which one a volume gave is not known here.
    constexpr std::string_view kShortName = "ddb~";
    if (segment.size() <= kShortName.size() || !str::iequals(segment.substr(0, kShortName.size()), kShortName))
        return false;
    const std::string_view number = segment.substr(kShortName.size());
    return std::all_of(number.begin(), number.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool isReservedSegment(const fs::path& segment) {
    return isReservedSegment(std::string_view(io::toUtf8(segment)));
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

namespace {

// The InvalidPathException of a rejection
[[noreturn]] void throwRejected(std::string_view path, std::string_view what, std::string_view code) {
    std::string message = "Invalid " + std::string(what);
    // A message is UTF-8 (DDBGetLastError): the bytes of a path that is not are left out
    if (utf8::isValid(path) && path.find('\0') == std::string_view::npos)
        message += " '" + std::string(path) + "'";
    throw InvalidPathException(message + ": " + std::string(code));
}

}  // namespace

std::string requireEntryPath(std::string_view path, std::string_view what, const ReservedRules& rules) {
    auto result = normalizeEntryPath(path, rules);
    if (!result.accepted())
        throwRejected(path, what, result.rejected);
    return std::move(result.path);
}

std::string optionalEntryPath(std::string_view path, std::string_view what) {
    return path.empty() ? std::string() : requireEntryPath(path, what);
}

std::string requireEntryFolder(std::string_view folder, std::string_view what) {
    if (folder.empty())
        return {};
    auto result = normalizeEntryPath(folder);
    if (result.accepted())
        return std::move(result.path);
    // Nothing left once the "." segments are dropped: the root
    if (result.rejected == kEmpty)
        return {};
    throwRejected(folder, what, result.rejected);
}

bool hasDotNotation(const std::string& path) {
    for (const std::string& segment : str::split(path, '/'))
        if (segment == ".." || segment == ".")
            return true;
    return false;
}

}  // namespace ddb
