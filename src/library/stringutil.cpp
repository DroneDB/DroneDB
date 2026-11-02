/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "stringutil.h"

namespace ddb::str {

std::vector<std::string> split(std::string_view s, std::string_view delimiter) {
    std::vector<std::string> pieces;

    // find("") matches at every position without advancing: the text is one piece
    if (delimiter.empty()) {
        pieces.emplace_back(s);
        return pieces;
    }

    size_t start = 0;
    for (size_t end; (end = s.find(delimiter, start)) != std::string_view::npos;
         start = end + delimiter.size())
        pieces.emplace_back(s.substr(start, end - start));
    pieces.emplace_back(s.substr(start));
    return pieces;
}

std::vector<std::string> split(std::string_view s, char delimiter) {
    return split(s, std::string_view(&delimiter, 1));
}

std::vector<std::string> splitWhitespace(std::string_view s) {
    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && isSpaceChar(s[i]))
            ++i;
        const size_t start = i;
        while (i < s.size() && !isSpaceChar(s[i]))
            ++i;
        if (i > start)
            tokens.emplace_back(s.substr(start, i - start));
    }
    return tokens;
}

std::string join(const std::vector<std::string>& items, std::string_view separator) {
    std::string result;
    for (size_t i = 0; i < items.size(); i++) {
        if (i > 0)
            result += separator;
        result += items[i];
    }
    return result;
}

void replaceAll(std::string& s, std::string_view from, std::string_view to) {
    if (from.empty())
        return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();  // 'to' may contain 'from' (replacing "x" with "yx")
    }
}

}  // namespace ddb::str
