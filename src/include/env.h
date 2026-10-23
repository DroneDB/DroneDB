/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef ENV_H
#define ENV_H

// Integer settings read from the environment (DQ-07), ddb::env: the one parser of the
// DDB_*_MAX_*, *_TIMEOUT_SECONDS, *_SIZE and *_MB variables. A value is a non-empty string of
// decimal digits in the accepted range: no sign, no space, no suffix ("12abc", "-1", "0x10" and
// "1e3" are invalid, where std::stoul read 12, a huge number, 0 and 1). An empty variable is
// unset.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "ddb_export.h"

namespace ddb::env {

// The largest value readPositiveInt accepts when the caller gives no limit
constexpr uint64_t kDefaultMaxInteger = INT32_MAX;

// A decimal integer in [min, max]: digits only. Nothing for anything else, including a value
// above max (it never overflows).
DDB_DLL std::optional<uint64_t> parseDecimal(std::string_view text, uint64_t min, uint64_t max);

struct Integer {
    enum class State {
        Unset,   // not defined, or empty
        Valid,   // value holds it
        Invalid  // defined, but not an integer of the accepted range (raw holds the text)
    };

    State state = State::Unset;
    uint64_t value = 0;
    std::string raw;
};

// Reads the variable `name` as an integer from min to max. Logs nothing.
DDB_DLL Integer readInteger(const char *name, uint64_t min, uint64_t max);

// readInteger(name, 1, max): a count or a number of seconds, where 0 makes no sense.
DDB_DLL Integer readPositiveInt(const char *name, uint64_t max = kDefaultMaxInteger);

// The value of the variable when it is a valid integer from min to max. An invalid value is
// logged at warning level ("Ignoring invalid NAME=...") and treated as unset: nothing.
DDB_DLL std::optional<uint64_t> readIntegerOrWarn(const char *name, uint64_t min, uint64_t max);

}  // namespace ddb::env

#endif  // ENV_H
