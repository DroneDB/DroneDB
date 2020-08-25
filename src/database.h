/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DATABASE_H
#define DATABASE_H

#include "sqlite_database.h"
#include "ddb_export.h"

namespace ddb{

class Database : public SqliteDatabase {
  public:
      DDB_DLL static void Initialize();
      DDB_DLL Database &createTables();
};

}

#endif // DATABASE_H
