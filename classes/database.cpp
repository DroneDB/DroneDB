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
#include <string>

#include "../logger.h"
#include "exceptions.h"
#include "database.h"
#include "../utils.h"

// Initialize spatialite
void Database::Initialize() {
    spatialite_init (0);
}

Database::Database() : db(nullptr) {}

Database &Database::open(const std::string &file) {
    if (db != nullptr) throw DBException("Can't open database " + file + ", one is already open (" + openFile + ")");
    LOGD << "Opening connection to " << file;
    if( sqlite3_open(file.c_str(), &db) != SQLITE_OK ) throw DBException("Can't open database: " + file);

//    if( sqlite3_enable_load_extension(db, 1) != SQLITE_OK ) throw DBException("Cannot enable load extension");
//    char *errMsg;
//    if( sqlite3_load_extension(db, (utils::getExeFolderPath() / "json1").c_str(), nullptr, &errMsg) == SQLITE_ERROR ) {
//        std::string error(errMsg);
//        sqlite3_free(errMsg);
//        throw DBException("Error during extension loading: " + error);
//    }

    this->openFile = file;

    return *this;
}

Database &Database::close() {
    if (db != nullptr) {
        LOGD << "Closing connection to " << openFile;
        sqlite3_close(db);
        db = nullptr;
    }

    return *this;
}

Database &Database::exec(const std::string &sql) {
    if (db == nullptr) throw DBException("Can't execute SQL: " + sql + ", db is not open");

    char *errMsg;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg) != SQLITE_OK ) {
        std::string error(errMsg);
        sqlite3_free(errMsg);
        throw SQLException(error);
    }

    return *this;
}

Database &Database::createTables() {
    std::string sql = R"<<<(
  SELECT InitSpatialMetaData(1, 'NONE');
  SELECT InsertEpsgSrid(4326);

  CREATE TABLE IF NOT EXISTS entries (
      path TEXT,
      hash TEXT,
      type INTEGER,
      meta TEXT,
      mtime INTEGER,
      size  INTEGER,
      depth INTEGER
  );
  SELECT AddGeometryColumn("entries", "point_geom", 4326, "POINTZ", "XYZ");
  SELECT AddGeometryColumn("entries", "polygon_geom", 4326, "POLYGONZ", "XYZ");

)<<<";

    LOGD << "About to create tables...";
    this->exec(sql);
    LOGD << "Created tables";

    return *this;
}

bool Database::tableExists(const std::string &table){
    auto q = query("SELECT count(*) FROM sqlite_master WHERE type='table' AND name=?");
    q->bind(1, table);

    if (q->fetch()){
        return q->getInt(0) == 1;
    }

    return false;
}

std::string Database::getOpenFile(){
    return openFile;
}

// @return  the number of rows modified, inserted or deleted by the
// most recently completed INSERT, UPDATE or DELETE statement
int Database::changes(){
    return sqlite3_changes(db);
}

std::unique_ptr<Statement> Database::query(const std::string &query){
    return std::make_unique<Statement>(db, query);
}

Database::~Database() {
    this->close();
}

