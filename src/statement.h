/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef STATEMENT_H
#define STATEMENT_H

#include <sqlite3.h>
#include <string>
#include "logger.h"
#include "ddb_export.h"

class Statement {
    sqlite3 *db;
    std::string query;

    bool hasRow;
    bool done;

    sqlite3_stmt *stmt;

    void bindCheck(int ret);
    Statement &step();
  public:
    DDB_DLL Statement(sqlite3 *db, const std::string &query);
    DDB_DLL ~Statement();

    DDB_DLL Statement &bind(int paramNum, const std::string &value);
    DDB_DLL Statement &bind(int paramNum, int value);
    DDB_DLL Statement &bind(int paramNum, long long value);

    DDB_DLL bool fetch();

    DDB_DLL int getInt(int columnId);
    DDB_DLL long long getInt64(int columnId);
    DDB_DLL std::string getText(int columnId);
    DDB_DLL double getDouble(int columnId);

    DDB_DLL int getColumnsCount() const;
    // TODO: more

    DDB_DLL void reset();
    DDB_DLL void execute();
};

#endif // STATEMENT_H
