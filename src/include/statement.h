/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef STATEMENT_H
#define STATEMENT_H

#include <sqlite3.h>
#include <string>
#include "logger.h"

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
  Statement(sqlite3 *db, const std::string &query);
  ~Statement();

  Statement(const Statement &) = delete;
  Statement &operator=(const Statement &) = delete;
  Statement(Statement &&other) noexcept;
  Statement &operator=(Statement &&other) noexcept;

  Statement &bind(int paramNum, const std::string &value);
  Statement &bind(int paramNum, int value);
  Statement &bind(int paramNum, long long value);
  Statement &bind(int paramNum, double value);

  bool fetch();

  // Readers of the current row: they do not change the statement
  int getInt(int columnId) const;
  long long getInt64(int columnId) const;
  std::string getText(int columnId) const;
  double getDouble(int columnId) const;

  void reset();
  void execute();

  std::string getQuery() const;
};

#endif // STATEMENT_H
