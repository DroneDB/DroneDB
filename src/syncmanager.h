/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef SYNCMANAGER_H
#define SYNCMANAGER_H

#include "dbops.h"
#include "ddb_export.h"
#include "entry.h"
#include "simpleentry.h"

namespace ddb {

#define SYNCFILE "sync.json"

class SyncManager {
    Database *db;

   public:
    SyncManager(Database *db) : db(db) {}

    DDB_DLL json getLastStamp(const std::string& registry = DEFAULT_REGISTRY);
    DDB_DLL void setLastStamp(const std::string& registry = DEFAULT_REGISTRY, Database *sourceDb = nullptr);

    DDB_DLL std::vector<SimpleEntry> getLastStampEntries(const std::string& registry = DEFAULT_REGISTRY);
};

}  // namespace ddb

#endif  // SYNCMANAGER_H
