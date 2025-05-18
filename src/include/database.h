/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DATABASE_H
#define DATABASE_H

#define DDB_BUILD_PATH "build"

#include "metamanager.h"
#include "sqlite_database.h"
#include "ddb_export.h"
#include "json.h"
#include "constants.h"

namespace ddb
{

  class Database : public SqliteDatabase
  {
  private:
    MetaManager *metaManager = nullptr;
    void *spatialiteCache = nullptr;

  public:
    DDB_DLL ~Database();
    DDB_DLL void afterOpen() override;
    DDB_DLL Database &createTables();
    DDB_DLL void ensureSchemaConsistency();

    DDB_DLL json getProperties() const;
    DDB_DLL json getStamp() const;

    DDB_DLL fs::path rootDirectory() const;
    DDB_DLL fs::path ddbDirectory() const;
    DDB_DLL fs::path tmpDirectory() const;
    DDB_DLL fs::path buildDirectory() const;

    DDB_DLL std::string getReadme() const;
    DDB_DLL json getExtent() const;

    DDB_DLL MetaManager *getMetaManager();
  };

  DDB_DLL json wktBboxCoordinates(const std::string &wktBbox);

}

#endif // DATABASE_H
