/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef STATEMENT_H
#define STATEMENT_H

#include <sqlite3.h>
#include <string>
#include "logger.h"

class Statement {
    sqlite3 *db;
    std::string query;

    bool hasRow;
    bool done;

    sqlite3_stmt *stmt;

    void bindCheck(int ret);
    Statement &step();
  public:
    Statement(sqlite3 *db, const std::string &query);
    ~Statement();

    Statement &bind(int paramNum, const std::string &value);
    Statement &bind(int paramNum, int value);
    Statement &bind(int paramNum, long long value);

    bool fetch();

    int getInt(int columnId);
    long long getInt64(int columnId);
    std::string getText(int columnId);
    double getDouble(int columnId);
    // TODO: more

    void reset();
    void execute();
};

#endif // STATEMENT_H
