/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <cassert>
#include <string>

#include "logger.h"
#include "exceptions.h"
#include "sqlite_database.h"
#include "utils.h"

namespace ddb{

SqliteDatabase::SqliteDatabase() : db(nullptr) {}

SqliteDatabase &SqliteDatabase::open(const std::string &file) {
    if (db != nullptr) throw DBException("Can't open database " + file + ", one is already open (" + openFile + ")");
    LOGD << "Opening connection to " << file;
    if( sqlite3_open(file.c_str(), &db) != SQLITE_OK ) throw DBException("Can't open database: " + file);

//    if( sqlite3_enable_load_extension(db, 1) != SQLITE_OK ) throw DBException("Cannot enable load extension");
//    char *errMsg;
//    if( sqlite3_load_extension(db, (getExeFolderPath() / "json1").c_str(), nullptr, &errMsg) == SQLITE_ERROR ) {
//        std::string error(errMsg);
//        sqlite3_free(errMsg);
//        throw DBException("Error during extension loading: " + error);
//    }

    this->openFile = file;
    this->afterOpen();

    return *this;
}

void SqliteDatabase::afterOpen(){
    // Nothing
}

SqliteDatabase &SqliteDatabase::close() {
    if (db != nullptr) {
        LOGD << "Closing connection to " << openFile;
        sqlite3_close(db);
        db = nullptr;
    }

    return *this;
}

SqliteDatabase &SqliteDatabase::exec(const std::string &sql) {
    if (db == nullptr) throw DBException("Can't execute SQL: " + sql + ", db is not open");

    char *errMsg;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg) != SQLITE_OK ) {
        std::string error(errMsg);
        sqlite3_free(errMsg);
        throw SQLException(error);
    }

    return *this;
}

bool SqliteDatabase::tableExists(const std::string &table){
    auto q = query("SELECT count(*) FROM sqlite_master WHERE type='table' AND name=?");
    q->bind(1, table);

    if (q->fetch()){
        return q->getInt(0) == 1;
    }

    return false;
}

std::string SqliteDatabase::getOpenFile(){
    return openFile;
}

// @return  the number of rows modified, inserted or deleted by the
// most recently completed INSERT, UPDATE or DELETE statement
int SqliteDatabase::changes(){
    return sqlite3_changes(db);
}

void SqliteDatabase::setJournalMode(const std::string &mode){
   this->exec("PRAGMA journal_mode=" + mode + ";");
}

std::unique_ptr<Statement> SqliteDatabase::query(const std::string &query) const{
    return std::make_unique<Statement>(db, query);
}

SqliteDatabase::~SqliteDatabase() {
    this->close();
}

}
