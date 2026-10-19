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

  class DatabasePool;

  class Database : public SqliteDatabase
  {
  private:
    MetaManager *metaManager = nullptr;
    void *spatialiteCache = nullptr;

    // Set when the connection comes from or goes back to the DatabasePool
    bool pooled = false;
    bool pooledNew = false;
    std::string poolKey;
    std::string poolFileId;
    sqlite3_int64 changesAtAcquire = 0;

    friend class DatabasePool;
    void attachPooled(sqlite3 *handle, void *cache, const std::string &file, const std::string &key, const std::string &fileId);
    void markPooled(const std::string &key, const std::string &fileId);
    sqlite3 *detachPooled(void *&cache, std::string &key, std::string &fileId, bool &wrote);

  public:
    DDB_DLL ~Database() override;

    /// Gives a connection opened by ddb::open() back to the DatabasePool, otherwise closes it
    DDB_DLL SqliteDatabase &close() override;
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
