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

json SyncManager::getLastStamp(const std::string &registry) {
    const auto path = this->db->ddbDirectory() / SYNCFILE;

    LOGD << "Path = " << path;
    LOGD << "Registry = " << registry;

    if (registry.length() == 0) 
        throw InvalidArgsException("Registry cannot be null");


    if (!exists(path)) {
        LOGD << "Path does not exist, creating empty file";
        std::ofstream out(path, std::ios_base::out);
        out << "{}";
        out.close();
    }

    std::ifstream i(path);
    json j;
    i >> j;

    //if (!j.contains(registry)) throw NoStampException("Tried to get last stamp for registry " + registry + " but found none");
    if (!j.contains(registry)){
        // Initialize
        setLastStamp(registry, this->db);
        return getLastStamp(registry);
    }else{
        return j[registry];
    }
}

void SyncManager::setLastStamp(const std::string& registry, Database *sourceDb) {
    if (sourceDb != nullptr){
        setLastStamp(registry, sourceDb->getStamp());
    }else{
        setLastStamp(registry, db->getStamp());
    }
}

void SyncManager::setLastStamp(const std::string &registry, const json &stamp){
    const auto path = this->db->ddbDirectory() / SYNCFILE;

    LOGD << "Path = " << path;
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

    j[registry] = stamp;

    std::ofstream out(path, std::ios_base::out | std::ios_base::trunc);
    out << j.dump(4);
    out.close();
}

}  // namespace ddb
