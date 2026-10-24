/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef TIMEUTIL_H
#define TIMEUTIL_H

#include <chrono>
#include <ctime>
#include <string>

namespace ddb {

// Seconds since the Unix epoch
inline time_t currentUnixTimestamp() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// A time (seconds since the Unix epoch) as the UTC date-time of RFC 3339 and ISO 8601
// ("2026-10-24T16:48:00Z")
inline std::string utcIso8601(time_t epochSeconds) {
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &epochSeconds);
#else
    gmtime_r(&epochSeconds, &utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buf;
}

}  // namespace ddb

#endif  // TIMEUTIL_H
