/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef SQLITEDATABASE_H
#define SQLITEDATABASE_H

// Spatialite includes
#include <geos_c.h>
#include <sqlite3.h>
#include <spatialite/gaiageo.h>
#include <spatialite.h>

#include <string>
#include <memory>

#include "statement.h"
#include "fs.h"

namespace ddb{

// Owns one SQLite connection (closed by the destructor). Neither copyable nor movable: a copy
// would close the connection twice, and a polymorphic class cannot be moved through a base
// reference without slicing off the state a derived class keeps for the connection (the
// SpatiaLite cache, the pool registration of Database). Hold it by unique_ptr to transfer it.
class SqliteDatabase {
  protected:
    sqlite3 *db = nullptr;
    std::string openFile;

    // Closes the SQLite connection (close() may be overridden to keep it)
    void closeConnection();
  public:
    SqliteDatabase();
    SqliteDatabase(const SqliteDatabase &) = delete;
    SqliteDatabase &operator=(const SqliteDatabase &) = delete;
    SqliteDatabase(SqliteDatabase &&) = delete;
    SqliteDatabase &operator=(SqliteDatabase &&) = delete;
    SqliteDatabase &open(const std::string &file);
    virtual void afterOpen();
    virtual SqliteDatabase &close();
    SqliteDatabase &reopen();
    SqliteDatabase &exec(const std::string &sql);
    bool tableExists(const std::string &table);
    std::string getOpenFile() const;
    int changes();
    std::string getJournalMode() const;
    void setJournalMode(const std::string &mode);
    void setWritableSchema(bool enabled);
    bool renameColumnIfExists(const std::string &table, const std::string &columnDefBefore, const std::string &columnDefAfter);

    // Schema versioning using PRAGMA user_version
    int getUserVersion() const;
    void setUserVersion(int version);

    // True when the connection cannot write the main database (a read-only file or
    // filesystem: SQLite then opens it read-only)
    bool isReadOnly() const;

    std::unique_ptr<Statement> query(const std::string &query) const;

    virtual ~SqliteDatabase();
};

}

#endif // SQLITEDATABASE_H
