/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef SYNCMANAGER_H
#define SYNCMANAGER_H

#include "dbops.h"
#include "ddb_export.h"
#include "entry.h"

namespace ddb {

#define SYNCFILE "sync.json"

class SyncManager {
    fs::path ddbFolder;

   public:
    SyncManager(const fs::path& ddbFolder) : ddbFolder(ddbFolder) {
        
    }

    DDB_DLL time_t getLastSync(const std::string& registry = DEFAULT_REGISTRY);
    DDB_DLL void setLastSync(const time_t time = 0,
                             const std::string& registry = DEFAULT_REGISTRY);
};

}  // namespace ddb

#endif  // SYNCMANAGER_H
