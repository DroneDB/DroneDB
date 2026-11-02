/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H

// What the library logs about itself and its environment when it starts (verbose mode).

#include <string>


namespace ddb::diagnostics {

// Logs the versions of libddb and of its libraries, the data paths and the locale
void printVersions();

// "Debug build ..." or "Release build ..." (from NDEBUG) and the compiler
std::string getBuildInfo();

// The value of an environment variable, or "<unset>" when it is not set (std::getenv
// returns null, which must never be streamed)
std::string getEnvOrUnset(const char* name);

}  // namespace ddb::diagnostics

#endif  // DIAGNOSTICS_H
