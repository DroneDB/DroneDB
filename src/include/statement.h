/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef STATEMENT_H
#define STATEMENT_H

#include <sqlite3.h>
#include <string>
#include "logger.h"
#include "ddb_export.h"

// Owns one prepared statement (finalized by the destructor). Move-only: a copy would
// finalize the same sqlite3_stmt twice. A moved-from Statement owns nothing and may only be
// destroyed or assigned to.
class Statement
{
  sqlite3 *db = nullptr;
  std::string query;

  bool hasRow = false;
  bool done = false;

  sqlite3_stmt *stmt = nullptr;

  void bindCheck(int ret);
  Statement &step();

public:
  DDB_DLL Statement(sqlite3 *db, const std::string &query);
  DDB_DLL ~Statement();

  Statement(const Statement &) = delete;
  Statement &operator=(const Statement &) = delete;
  DDB_DLL Statement(Statement &&other) noexcept;
  DDB_DLL Statement &operator=(Statement &&other) noexcept;

  DDB_DLL Statement &bind(int paramNum, const std::string &value);
  DDB_DLL Statement &bind(int paramNum, int value);
  DDB_DLL Statement &bind(int paramNum, long long value);
  DDB_DLL Statement &bind(int paramNum, double value);

  DDB_DLL bool fetch();

  // Readers of the current row: they do not change the statement
  DDB_DLL int getInt(int columnId) const;
  DDB_DLL long long getInt64(int columnId) const;
  DDB_DLL std::string getText(int columnId) const;
  DDB_DLL double getDouble(int columnId) const;
  DDB_DLL const void *getBlob(int columnId) const;

  DDB_DLL int getColumnsCount() const;
  // TODO: more

  DDB_DLL void reset();
  DDB_DLL void execute();

  DDB_DLL std::string getQuery() const;
};

#endif // STATEMENT_H
