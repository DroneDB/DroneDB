/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef SYNCMANAGER_H
#define SYNCMANAGER_H

#include "dbops.h"
#include "entry.h"
#include "simpleentry.h"

namespace ddb
{

#define SYNCFILE "sync.json"

    class SyncManager
    {
        Database *db;

    public:
        SyncManager(Database *db) : db(db) {}

        json getLastStamp(const std::string &registry = DEFAULT_REGISTRY);
        void setLastStamp(const std::string &registry = DEFAULT_REGISTRY, Database *sourceDb = nullptr);
        void setLastStamp(const std::string &registry, const json &stamp);
    };

} // namespace ddb

#endif // SYNCMANAGER_H
