/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DATABASE_H
#define DATABASE_H

#include "ddb_export.h"
#include "sqlite_database.h"

namespace ddb {

class Database : public SqliteDatabase {
   public:
    DDB_DLL static void Initialize();
    DDB_DLL Database &createTables();
};

}  // namespace ddb

#endif  // DATABASE_H
