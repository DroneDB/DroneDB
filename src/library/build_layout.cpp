/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The build key and the names of the pending markers (see build_layout.h).

#include "build_layout.h"

#include "hash.h"
#include "json.h"

namespace ddb {

std::string buildKey(const std::string& hash, const json* georef) {
    if (georef == nullptr || !georef->is_object() || georef->empty())
        return hash;
    // nlohmann::json keeps object members sorted, so one georeference has one text
    return hash + BuildKeySeparator + Hash::strSHA256(georef->dump()).substr(0, GeorefDigestLength);
}

std::string entryPathId(const std::string& entryPath) {
    return Hash::strSHA256(entryPath).substr(0, PathIdLength);
}

std::string pendingMarkerName(const std::string& key, const std::string& entryPath) {
    return key + "." + entryPathId(entryPath) + PendingMarkerExtension;
}

std::optional<PendingMarkerName> parsePendingMarkerName(std::string_view fileName) {
    const std::string_view extension = PendingMarkerExtension;
    if (fileName.size() <= extension.size() || !fileName.ends_with(extension))
        return std::nullopt;
    const std::string_view stem = fileName.substr(0, fileName.size() - extension.size());

    // "<hash>"
    if (isHashLike(stem))
        return PendingMarkerName{std::string(stem), {}};

    // "<key>.<pathId>"
    if (stem.size() <= PathIdLength + 1 || stem[stem.size() - PathIdLength - 1] != '.')
        return std::nullopt;
    const std::string_view key = stem.substr(0, stem.size() - PathIdLength - 1);
    const std::string_view pathId = stem.substr(stem.size() - PathIdLength);
    if (!isBuildKey(key))
        return std::nullopt;
    for (const char c : pathId)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return std::nullopt;
    return PendingMarkerName{std::string(key), std::string(pathId)};
}

}  // namespace ddb
