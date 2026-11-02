/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "env.h"

#include "logger.h"
#include "mio.h"

namespace ddb::env {

std::optional<uint64_t> parseDecimal(std::string_view text, uint64_t min, uint64_t max) {
    if (text.empty())
        return std::nullopt;
    uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9')
            return std::nullopt;
        const auto digit = static_cast<uint64_t>(c - '0');
        if (digit > max || value > (max - digit) / 10)
            return std::nullopt;  // above max (and never overflows)
        value = value * 10 + digit;
    }
    return value >= min ? std::optional<uint64_t>(value) : std::nullopt;
}

Integer readInteger(const char *name, uint64_t min, uint64_t max) {
    Integer result;
    const auto raw = io::getEnvUtf8(name);
    if (!raw || raw->empty())
        return result;
    result.raw = *raw;
    if (const auto value = parseDecimal(result.raw, min, max)) {
        result.state = Integer::State::Valid;
        result.value = *value;
    } else {
        result.state = Integer::State::Invalid;
    }
    return result;
}

Integer readPositiveInt(const char *name, uint64_t max) {
    return readInteger(name, 1, max);
}

std::optional<uint64_t> readIntegerOrWarn(const char *name, uint64_t min, uint64_t max) {
    const Integer value = readInteger(name, min, max);
    if (value.state == Integer::State::Invalid) {
        LOGW << "Ignoring invalid " << name << "=\"" << value.raw << "\" (expected an integer from " << min
             << " to " << max << ")";
        return std::nullopt;
    }
    if (value.state == Integer::State::Unset)
        return std::nullopt;
    return value.value;
}

}  // namespace ddb::env
