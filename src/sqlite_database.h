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
#include "ddb_export.h"

namespace ddb{

class SqliteDatabase {
    sqlite3 *db;
    std::string openFile;
  public:
    DDB_DLL SqliteDatabase();
    DDB_DLL SqliteDatabase &open(const std::string &file);
    DDB_DLL SqliteDatabase &close();
    DDB_DLL SqliteDatabase &exec(const std::string &sql);
    DDB_DLL bool tableExists(const std::string &table);
    DDB_DLL std::string getOpenFile();
    DDB_DLL int changes();

    DDB_DLL std::unique_ptr<Statement> query(const std::string &query);

    DDB_DLL ~SqliteDatabase();
};

}

#endif // SQLITEDATABASE_H
