/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DATABASE_H
#define DATABASE_H

#define DDB_BUILD_PATH "build"

// Database schema version for migration management.
// Increment this value when making schema changes.
// Version 0 = legacy database without versioning
// Version 1 = first versioned schema (current)
#define DDB_SCHEMA_VERSION 1

#include "metamanager.h"
#include "sqlite_database.h"
#include "json_fwd.h"
#include "constants.h"

#include <functional>
#include <memory>

namespace ddb
{

  class DatabasePool;

  /// Neither copyable nor movable (see SqliteDatabase): the MetaManager points back to this
  /// object and DatabasePool hands the connection over by reference
  class Database : public SqliteDatabase
  {
  private:
    std::unique_ptr<MetaManager> metaManager;
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

    void migrateLegacySchema(int dbVersion);

  public:
    /// Busy timeout of every index connection: only a floor for statements run outside an
    /// explicit Transaction; RetryPolicy owns the wait budget (retrypolicy.h)
    static constexpr int busyTimeoutMs = 200;

    Database() = default;
    ~Database() override;
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;
    Database(Database &&) = delete;
    Database &operator=(Database &&) = delete;

    /// Gives a connection opened by ddb::open() back to the DatabasePool, otherwise closes it
    SqliteDatabase &close() override;
    void afterOpen() override;
    Database &createTables();
    void ensureSchemaConsistency();
    /// Removes the entry with an empty path that older versions wrote for the dataset root
    /// (`ddb add .`). Best effort: an index that cannot be written keeps it.
    void removeRootEntry();

    json getProperties() const;
    json getStamp() const;
    /// The stamp of an index with nothing in it: what a Registry reports for a dataset nobody has
    /// pushed to, and the baseline of a dataset that was never synced with one
    static json emptyStamp();
    /// getStamp().dump(), without building the json document
    std::string getStampJson() const;
    /// getStamp()["checksum"], without collecting the entries
    std::string getStampChecksum() const;

    fs::path rootDirectory() const;
    fs::path ddbDirectory() const;
    fs::path tmpDirectory() const;
    fs::path buildDirectory() const;

    std::string getReadme() const;
    json getExtent() const;

    /// Created on first use; owned by this Database, which outlives the returned pointer
    MetaManager *getMetaManager();
  };

  /// Frees the SpatiaLite cache of a connection (spatialite_cleanup_ex) once the connection
  /// is closed. Every cache is freed through here: one at a time in the process (database.cpp).
  void freeSpatialiteCache(void *cache);

  json wktBboxCoordinates(const std::string &wktBbox);

}

#endif // DATABASE_H
