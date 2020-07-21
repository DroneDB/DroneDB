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

class SqliteDatabase {
    sqlite3 *db;
    std::string openFile;
  public:
    SqliteDatabase();
    SqliteDatabase &open(const std::string &file);
    SqliteDatabase &close();
    SqliteDatabase &exec(const std::string &sql);
    bool tableExists(const std::string &table);
    std::string getOpenFile();
    int changes();

    std::unique_ptr<Statement> query(const std::string &query);

    ~SqliteDatabase();
};

}

#endif // SQLITEDATABASE_H
