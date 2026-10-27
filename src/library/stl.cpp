/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "stl.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#include "logger.h"
#include "mio.h"

namespace ddb {

namespace {

constexpr uintmax_t BinaryHeaderSize = 80;    // free text
constexpr uintmax_t BinaryCountSize = 4;      // uint32 triangle count
constexpr uintmax_t BinaryTriangleSize = 50;  // normal + 3 vertices + 2 attribute bytes
constexpr size_t AsciiProbeSize = 64 * 1024;  // far more than "solid <name>" and "facet normal"

bool isBinaryStl(std::ifstream& in, uintmax_t fileSize) {
    if (fileSize < BinaryHeaderSize + BinaryCountSize)
        return false;

    std::array<unsigned char, BinaryCountSize> count{};
    in.seekg(static_cast<std::streamoff>(BinaryHeaderSize));
    if (!in.read(reinterpret_cast<char*>(count.data()), static_cast<std::streamsize>(count.size())))
        return false;

    const uintmax_t triangles =
        static_cast<uintmax_t>(count[0]) | (static_cast<uintmax_t>(count[1]) << 8) |
        (static_cast<uintmax_t>(count[2]) << 16) | (static_cast<uintmax_t>(count[3]) << 24);

    return triangles > 0 &&
           fileSize == BinaryHeaderSize + BinaryCountSize + triangles * BinaryTriangleSize;
}

bool isSpace(char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

// Takes the next white-space separated token off the front of text (empty at the end)
std::string_view nextToken(std::string_view& text) {
    while (!text.empty() && isSpace(text.front()))
        text.remove_prefix(1);
    size_t len = 0;
    while (len < text.size() && !isSpace(text[len]))
        ++len;
    const std::string_view token = text.substr(0, len);
    text.remove_prefix(len);
    return token;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) ==
               std::tolower(static_cast<unsigned char>(y));
    });
}

bool isAsciiStl(std::ifstream& in) {
    std::string head(AsciiProbeSize, '\0');
    in.clear();
    in.seekg(0);
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<size_t>(in.gcount()));

    std::string_view text(head);
    if (text.starts_with("\xEF\xBB\xBF"))
        text.remove_prefix(3);

    if (!equalsIgnoreCase(nextToken(text), "solid"))
        return false;

    // The rest of the line is the name of the solid: it can hold anything, even "facet"
    const size_t endOfLine = text.find_first_of("\r\n");
    if (endOfLine == std::string_view::npos)
        return false;
    text.remove_prefix(endOfLine);

    return equalsIgnoreCase(nextToken(text), "facet") &&
           equalsIgnoreCase(nextToken(text), "normal");
}

}  // namespace

EntryType identifyStl(const fs::path& stlFile) {
    try {
        std::error_code ec;
        const uintmax_t fileSize = fs::file_size(stlFile, ec);
        if (ec)
            return EntryType::Generic;

        std::ifstream in(stlFile, std::ios::binary);
        if (!in.is_open())
            return EntryType::Generic;

        if (isBinaryStl(in, fileSize) || isAsciiStl(in))
            return EntryType::Model;

        LOGD << io::toUtf8(stlFile) << " is neither a binary nor an ASCII STL, treating as generic";
    } catch (const std::exception& e) {
        LOGD << "Cannot classify " << io::toUtf8(stlFile) << ", not a readable STL: " << e.what();
    }
    return EntryType::Generic;
}

}  // namespace ddb
