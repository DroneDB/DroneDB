/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef METAMANAGER_H
#define METAMANAGER_H

#include "database.h"
#include "json.h"

namespace ddb {

class MetaManager {
    Database* db;

    std::string entryPath(const std::string &path) const;

    std::string getKey(const std::string &key, bool isList) const;
    json getMetaJson(Statement *q) const;
    json getMetaJson(const std::string &q) const;
    std::string validateData(const std::string &data) const;
public:
    MetaManager(ddb::Database* db) : db(db) {}


    DDB_DLL json add(const std::string& key, const std::string &data, const std::string &path = "");
};

}

#endif // METAMANAGER_H
