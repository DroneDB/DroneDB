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

struct CopyAction {
    std::string source;
    std::string destination;

    CopyAction(std::string source, std::string destination) {
        this->source = std::move(source);
        this->destination = std::move(destination);
    }

    std::string toString() const {
        return "CPY -> " + source + " TO " + destination;
    }
};

struct RemoveAction {
    std::string path;
    bool directory;

    RemoveAction(std::string path, bool directory) {
        this->path = std::move(path);
        this->directory = directory;
    }

    std::string toString() const {
        return std::string("DEL -> [") + (directory ? "D" : "F") + "] " + path;
    }
};

struct AddAction {
    std::string path;
    bool directory;

    AddAction(std::string path, bool directory) {
        this->path = std::move(path);
        this->directory = directory;
    }

    std::string toString() const {
        return std::string("ADD -> [") + (directory ? "D" : "F") + "] " + path;
    }
};

struct Delta {
    std::vector<AddAction> adds;
    std::vector<RemoveAction> removes;
    std::vector<CopyAction> copies;

    DDB_DLL std::vector<std::string> modifiedPathList() const{
        std::vector<std::string> result;
        for (unsigned long i = 0; i < adds.size(); i++) result.push_back(adds[i].path);
        for (unsigned long i = 0; i < removes.size(); i++) result.push_back(removes[i].path);
        for (unsigned long i = 0; i < copies.size(); i++) result.push_back(copies[i].destination);
        return result;
    }
};

DDB_DLL void to_json(json& j, const SimpleEntry& e);
DDB_DLL void to_json(json& j, const CopyAction& e);
DDB_DLL void to_json(json& j, const RemoveAction& e);
DDB_DLL void to_json(json& j, const AddAction& e);
DDB_DLL void to_json(json& j, const Delta& d);

DDB_DLL Delta getDelta(std::vector<ddb::SimpleEntry> source,
                       std::vector<ddb::SimpleEntry> destination);

DDB_DLL Delta getDelta(Database* sourceDb, Database* targetDb);

DDB_DLL Delta getDelta(Database *sourceDb, std::vector<ddb::SimpleEntry> destination);
    


}  // namespace ddb

#endif  // DELTA_H
