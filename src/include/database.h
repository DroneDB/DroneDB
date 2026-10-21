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

#include <functional>

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
    std::string poolKey;
    std::string poolFileId;

    friend class DatabasePool;
    void attachPooled(sqlite3 *handle, void *cache, const std::string &file, const std::string &key, const std::string &fileId);
    void markPooled(const std::string &key, const std::string &fileId);
    sqlite3 *detachPooled(void *&cache, std::string &key, std::string &fileId);

    using StampEntryVisitor = std::function<void(const std::string &path, const std::string &hash)>;
    using StampMetaVisitor = std::function<void(const std::string &id)>;
    // Reads the rows of the stamp in stamp order and returns its checksum
    std::string readStamp(const StampEntryVisitor &onEntry, const StampMetaVisitor &onMeta) const;

  public:
    /// Busy timeout of every index connection: only a floor for statements run outside an
    /// explicit Transaction; RetryPolicy owns the wait budget (retrypolicy.h)
    static constexpr int busyTimeoutMs = 200;

    DDB_DLL ~Database() override;

    /// Gives a connection opened by ddb::open() back to the DatabasePool, otherwise closes it
    DDB_DLL SqliteDatabase &close() override;
    DDB_DLL void afterOpen() override;
    DDB_DLL Database &createTables();
    DDB_DLL void ensureSchemaConsistency();
    /// Removes the entry with an empty path that older versions wrote for the dataset root
    /// (`ddb add .`). Best effort: an index that cannot be written keeps it.
    DDB_DLL void removeRootEntry();

    DDB_DLL json getProperties() const;
    DDB_DLL json getStamp() const;
    /// getStamp().dump(), without building the json document
    DDB_DLL std::string getStampJson() const;
    /// getStamp()["checksum"], without collecting the entries
    DDB_DLL std::string getStampChecksum() const;

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
