/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DELTA_H
#define DELTA_H

#include <iostream>


#include "ddb_export.h"
#include "dbops.h"
#include "utils.h"


namespace ddb {

struct SimpleEntry {
    std::string path;
    std::string hash;
    EntryType type;

    std::string toString() const {
        return path + " - " + hash + " (" + typeToHuman(type) + ")";
    }

    SimpleEntry(std::string path, std::string hash, EntryType type) {
        this->path = std::move(path);
        this->hash = std::move(hash);
        this->type = type;
    }
};

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
    EntryType type;

    RemoveAction(std::string path, ddb::EntryType type) {
        this->path = std::move(path);
        this->type = type;
    }

    std::string toString() const {
        return "DEL -> [" + typeToHuman(type) + "] " + path;
    }
};

struct AddAction {
    std::string path;
    EntryType type;

    AddAction(std::string path, ddb::EntryType type) {
        this->path = std::move(path);
        this->type = type;
    }

    std::string toString() const {
        return "ADD -> [" + typeToHuman(type) + "] " + path;
    }
};

struct Delta {
    std::vector<AddAction> adds;
    std::vector<RemoveAction> removes;
    std::vector<CopyAction> copies;
};

DDB_DLL Delta getDelta(std::vector<ddb::SimpleEntry> source,
                       std::vector<ddb::SimpleEntry> destination);

}  // namespace ddb

#endif  // DELTA_H
