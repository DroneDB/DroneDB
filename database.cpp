/* Copyright 2019 MasseranoLabs LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include <cassert>
#include "logger.h"
#include "exceptions.h"
#include "database.h"

// Initialize spatialite
void Database::Initialize() {
    spatialite_init (0);
}

Database::Database() : db(nullptr) {}

void Database::open(const std::string &file) {
    if (db != nullptr) throw DBException("Can't open database " + file + ", one is already open (" + open_file + ")");
    LOGD << "DATABASE: Opening connection to " << file;
    if( sqlite3_open(file.c_str(), &db) ) throw DBException("Can't open database: " + file);
    this->open_file = file;
}

// char *zErrMsg = nullptr;

void Database::close() {
    if (db != nullptr) {
        LOGD << "DATABASE: Closing connection to " << open_file;
        sqlite3_close(db);
        db = nullptr;
    }
}

Database::~Database() {
    this->close();
}

