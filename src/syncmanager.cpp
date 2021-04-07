/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "syncmanager.h"

#include <ddb.h>
#include <mio.h>

#include "dbops.h"
#include "exceptions.h"
#include "registryutils.h"

namespace ddb {

time_t SyncManager::getLastSync(const std::string& registry) {
    const auto path = this->ddbFolder / SYNCFILE;

    LOGD << "Path = " << path;
    LOGD << "Registry = " << registry;

    if (registry.length() == 0) 
        throw InvalidArgsException("Registry cannot be null");


    if (!exists(path)) {
        LOGD << "Path does not exist, creating empty file";

        std::ofstream out(path, std::ios_base::out);
        out << "{}";
        out.close();
        return 0;
    }

    std::ifstream i(path);
    json j;
    i >> j;

    LOGD << "Contents: " << j.dump();

    if (!j.contains(registry)) return 0;

    std::time_t t = j[registry];

    return t;
}

void SyncManager::setLastSync(
    const time_t t, const std::string& registry
                              ) {
    const auto path = this->ddbFolder / SYNCFILE;

    LOGD << "Path = " << path;
    LOGD << "Time = " << t;
    LOGD << "Registry = " << registry;

    if (registry.length() == 0) 
        throw InvalidArgsException("Registry cannot be null");
    
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

    j[registry] = t != 0 ? t : time(nullptr);

    if (exists(path)) {
        fs::remove(path);
    }

    std::ofstream out(path, std::ios_base::out);
    out << std::setw(4) << j;
    out.close();
}
}  // namespace ddb