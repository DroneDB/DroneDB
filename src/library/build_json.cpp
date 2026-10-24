/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// The JSON shapes of the build outputs of the C API (DDBGetPendingBuildInfo, DDBGetBuildStatus,
// DDBCleanup), frozen by the contract fixtures (tests/contract).
#include "build.h"
#include "json.h"

namespace ddb {

void to_json(json &j, const PendingBuildInfo &info) {
    j = json{{"hash", info.hash},
             {"path", info.path},
             {"missingDependencies", info.missingDependencies},
             {"lastAttempt", info.lastAttempt},
             {"retryAfter", info.retryAfter}};
}

void to_json(json &j, const BuildStatus &status) {
    j = json{{"path", status.path},
             {"exists", status.exists},
             {"buildable", status.buildable},
             {"complete", status.complete}};
}

void to_json(json &j, const CleanupResult &result) {
    j = json{{"entries", result.removedEntries}, {"builds", result.removedBuilds}};
}

}  // namespace ddb
