/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "utf8.h"

namespace ddb::utf8 {

bool decode(std::string_view s, size_t& i, char32_t& cp) noexcept {
    if (i >= s.size())
        return false;

    const auto byte = [&s](size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char lead = byte(i);
    if (lead < 0x80) {
        cp = lead;
        i++;
        return true;
    }

    size_t length;
    char32_t value;
    char32_t minimum;  // the smallest code point of this length: a smaller one is overlong
    if ((lead & 0xE0) == 0xC0) {
        length = 2;
        value = lead & 0x1F;
        minimum = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        value = lead & 0x0F;
        minimum = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        value = lead & 0x07;
        minimum = 0x10000;
    } else {
        return false;  // a continuation byte, or 0xF8..0xFF
    }

    if (s.size() - i < length)
        return false;
    for (size_t k = 1; k < length; k++) {
        const unsigned char next = byte(i + k);
        if ((next & 0xC0) != 0x80)
            return false;
        value = (value << 6) | (next & 0x3F);
    }

    if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
        return false;

    cp = value;
    i += length;
    return true;
}

bool isValid(std::string_view s) noexcept {
    size_t i = 0;
    char32_t cp;
    while (i < s.size())
        if (!decode(s, i, cp))
            return false;
    return true;
}

}  // namespace ddb::utf8
