/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef METAMANAGER_H
#define METAMANAGER_H

#include "json.h"
#include "ddb_export.h"
#include "statement.h"

namespace ddb {

class Database;

class MetaManager {
    Database* db;

    std::string entryPath(const std::string &path, const std::string &cwd = "") const;

    std::string getKey(const std::string &key, bool isList) const;
    json getMetaJson(Statement *q) const;
    json metaStmtToJson(Statement *q) const;
    json getMetaJson(const std::string &q) const;
    std::string validateData(const std::string &data) const;
    bool isList(const std::string &key) const;
public:
    MetaManager(ddb::Database* db) : db(db) {}

    // Core
    DDB_DLL json add(const std::string& key, const std::string &data, const std::string &path = "", const std::string &cwd = "");
    DDB_DLL json set(const std::string& key, const std::string &data, const std::string &path = "", const std::string &cwd = "");
    DDB_DLL json remove(const std::string& id);
    DDB_DLL json get(const std::string& key, const std::string &path = "", const std::string &cwd = "");
    DDB_DLL json unset(const std::string& key, const std::string &path = "", const std::string &cwd = "");
    DDB_DLL json list(const std::string &path = "", const std::string &cwd = "") const;
    DDB_DLL json dump(const json &ids = json::array());
    DDB_DLL json restore(const json &metaDump);
    DDB_DLL json bulkRemove(const std::vector<std::string> &ids);

    // Helpers
    DDB_DLL std::string getString(const std::string& key, const std::string &path = "", const std::string &cwd = "", const std::string &defaultValue = "");
};

}

#endif // METAMANAGER_H
