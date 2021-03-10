/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "tagmanager.h"

#include <ddb.h>
#include <mio.h>

#include "dbops.h"
#include "exceptions.h"

namespace ddb {

std::string TagManager::getTag(const std::string& registry) {
    const auto path = this->ddbFolder / DDB_FOLDER / TAGSFILE;

    LOGD << "Path = " << path;
    LOGD << "Getting tag of registry " << registry;

    if (!exists(path)) {
        LOGD << "Path does not exist, creating empty file";

        std::ofstream out(path, std::ios_base::out);
        out << "{}";
        out.close();
        return "";
    }

    std::ifstream i(path);
    json j;
    i >> j;

    LOGD << "Contents: " << j.dump();

    if (!j.contains(registry)) return "";

    const auto reg = j[registry];

    if (j.is_null()) return "";

    return reg["tag"];
}
void TagManager::setTag(const std::string& tag, const std::string& registry) {
    const auto path = this->ddbFolder / DDB_FOLDER / TAGSFILE;

    LOGD << "Path = " << path;
    LOGD << "Setting tag '" << tag << "' of registry " << registry;

    if (!exists(path)) {
        std::ofstream out(path, std::ios_base::out);
        out << "{}";
        out.close();
    }

    std::ifstream i(path);
    json j;
    i >> j;
    i.close();

    LOGD << "Contents: " << j.dump();

    // if (reg.is_null()) {
    j[registry] = {{"tag", tag},
                   {"mtime", std::chrono::system_clock::to_time_t(
                                 std::chrono::system_clock::now())}};

    if (exists(path)) {
        fs::remove(path);
    }

    std::ofstream out(path, std::ios_base::out);
    out << std::setw(4) << j;
    out.close();
}
}  // namespace ddb
