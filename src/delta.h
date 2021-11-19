/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DELTA_H
#define DELTA_H

#include <iostream>

#include "dbops.h"
#include "ddb_export.h"
#include "utils.h"
#include "simpleentry.h"

namespace ddb {

struct RemoveAction {
    std::string path;
    std::string hash;

    RemoveAction(const std::string &path, const std::string &hash) {
        this->path = path;
        this->hash = hash;
    }

    std::string toString() const {
        return std::string("DEL -> [") + (isDirectory() ? "D" : "F") + "] " + path;
    }

    bool isDirectory() const {
        return this->hash.empty();
    }
};

struct AddAction {
    std::string path;
    std::string hash;

    AddAction(const std::string &path, const std::string &hash) {
        this->path = path;
        this->hash = hash;
    }

    std::string toString() const {
        return std::string("ADD -> [") + (isDirectory() ? "D" : "F") + "] " + path;
    }

    bool isDirectory() const {
        return this->hash.empty();
    }
};

struct Delta {
    std::vector<AddAction> adds;
    std::vector<RemoveAction> removes;

    DDB_DLL std::vector<std::string> modifiedPathList() const{
        std::vector<std::string> result;
        for (unsigned long i = 0; i < adds.size(); i++) result.push_back(adds[i].path);
        for (unsigned long i = 0; i < removes.size(); i++) result.push_back(removes[i].path);
        return result;
    }
};

DDB_DLL void to_json(json& j, const SimpleEntry& e);
DDB_DLL void to_json(json& j, const RemoveAction& e);
DDB_DLL void to_json(json& j, const AddAction& e);
DDB_DLL void to_json(json& j, const Delta& d);

DDB_DLL Delta getDelta(std::vector<ddb::SimpleEntry> source,
                       std::vector<ddb::SimpleEntry> destination);

DDB_DLL Delta getDelta(Database* sourceDb, Database* targetDb);

DDB_DLL Delta getDelta(Database *sourceDb, std::vector<ddb::SimpleEntry> destination);
    


}  // namespace ddb

#endif  // DELTA_H
