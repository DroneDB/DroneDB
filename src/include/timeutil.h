/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef TIMEUTIL_H
#define TIMEUTIL_H

#include <chrono>
#include <ctime>

namespace ddb {

// Seconds since the Unix epoch
inline time_t currentUnixTimestamp() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace ddb

#endif  // TIMEUTIL_H
