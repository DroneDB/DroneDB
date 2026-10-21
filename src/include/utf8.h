/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef UTF8_H
#define UTF8_H

#include <cstddef>
#include <string_view>

#include "ddb_export.h"

// UTF-8 decoding for the code that compares or validates names (entry paths, name patterns).
// Strict RFC 3629: an overlong form, a surrogate (U+D800..U+DFFF), a code point above U+10FFFF,
// a stray continuation byte or a truncated sequence is invalid.
namespace ddb::utf8 {

/// Decodes the code point that starts at s[i] into cp and moves i past it. False, with i and cp
/// unchanged, when s[i] does not start a valid sequence (or i is at the end).
DDB_DLL bool decode(std::string_view s, size_t& i, char32_t& cp) noexcept;

/// True when s is valid UTF-8 (the empty string is)
DDB_DLL bool isValid(std::string_view s) noexcept;

}  // namespace ddb::utf8

#endif  // UTF8_H
