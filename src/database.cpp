/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "database.h"

#include <fstream>
#include <string>

#include "exceptions.h"
#include "logger.h"

namespace ddb {

// Initialize spatialite
void Database::Initialize() { spatialite_init(0); }

void Database::afterOpen() {
    this->setJournalMode("wal");

    // If table is locked, sleep up to 30 seconds
    if (sqlite3_busy_timeout(db, 30000) != SQLITE_OK) {
        LOGD << "Cannot set busy timeout";
    }
}

const char *entriesTableDdl = R"<<<(
  SELECT InitSpatialMetaData(1, 'NONE');
  SELECT InsertEpsgSrid(4326);

  CREATE TABLE IF NOT EXISTS entries (
      path TEXT PRIMARY KEY,
      hash TEXT,
      type INTEGER,
      properties TEXT,
      mtime INTEGER,
      size  INTEGER,
      depth INTEGER
  );
  SELECT AddGeometryColumn("entries", "point_geom", 4326, "POINTZ", "XYZ");
  SELECT AddGeometryColumn("entries", "polygon_geom", 4326, "POLYGONZ", "XYZ");

  CREATE INDEX IF NOT EXISTS ix_entries_type
  ON entries (type);
)<<<";

const char *passwordsTableDdl = R"<<<(
  CREATE TABLE IF NOT EXISTS passwords (
      salt TEXT,
      hash TEXT      
  );
)<<<";

const char *attributesTableDdl = R"<<<(

  CREATE TABLE IF NOT EXISTS attributes (
      name TEXT NOT NULL PRIMARY KEY,
      ivalue INTEGER,
      rvalue REAL,
      tvalue TEXT,
      bvalue BLOB
  );
)<<<";

const char *entriesMetaTableDdl = R"<<<(
  CREATE TABLE IF NOT EXISTS entries_meta (
      id TEXT PRIMARY KEY,
      path TEXT NOT NULL,
      key TEXT NOT NULL,
      data TEXT NOT NULL,
      mtime INTEGER NOT NULL
  );
  CREATE INDEX IF NOT EXISTS ix_entries_meta_path
  ON entries_meta (path);
  CREATE INDEX IF NOT EXISTS ix_entries_meta_key
  ON entries_meta (key);

CREATE TRIGGER tg_entries_meta_autouuid
AFTER INSERT ON entries_meta
FOR EACH ROW
WHEN (NEW.id IS NULL)
BEGIN
   UPDATE entries_meta SET id = (select lower(hex( randomblob(4)) || '-' || hex( randomblob(2))
             || '-' || '4' || substr( hex( randomblob(2)), 2) || '-'
             || substr('AB89', 1 + (abs(random()) % 4) , 1)  ||
             substr(hex(randomblob(2)), 2) || '-' || hex(randomblob(6))) ) WHERE rowid = NEW.rowid;
END;
)<<<";

Database &Database::createTables() {
    const std::string sql = std::string(entriesTableDdl) + '\n' +
                            passwordsTableDdl + '\n' + attributesTableDdl;

    LOGD << "About to create tables...";
    this->exec(sql);
    LOGD << "Created tables";

    return *this;
}

DDB_DLL void Database::ensureSchemaConsistency() {

    LOGD << "Ensuring schema consistency";

    if (!this->tableExists("entries")) {
        LOGD << "Entries table does not exist, creating it";
        this->exec(entriesTableDdl);
        LOGD << "Entries table created";
    }

    if (!this->tableExists("passwords")) {
        LOGD << "Passwords table does not exist, creating it";
        this->exec(passwordsTableDdl);
        LOGD << "Passwords table created";
    }

    if (!this->tableExists("attributes")) {
        LOGD << "Attributes table does not exist, creating it";
        this->exec(attributesTableDdl);
        LOGD << "Attributes table created";
    }

    if (!this->tableExists("entries_meta")){
        LOGD << "Entries meta table does not exist, creating it";
        this->exec(entriesMetaTableDdl);
        LOGD << "Entries meta table created";
    }

    // Migration from 0.9.11 to 0.9.12 (can be removed in the near future)
    // where we renamed "entries.meta" --> "entries.properties"
    // TODO: remove me in 2022
    if (this->renameColumnIfExists("entries", "meta TEXT", "properties TEXT")){
        this->reopen();
    }
}

void Database::setPublic(bool isPublic) {

    if (this->hasAttribute("public") && 
        this->getBoolAttribute("public") == isPublic) return;

    this->setBoolAttribute("public", isPublic);
    this->setLastUpdate();
}

bool Database::isPublic() const {
    return this->hasAttribute("public") ? this->getBoolAttribute("public")
                                        : false;
}

time_t Database::getLastUpdate() const {
    return static_cast<time_t>(this->getLongAttribute("mtime"));
}

void Database::setLastUpdate(const time_t mtime) {
    this->setLongAttribute("mtime", mtime);
}

void Database::chattr(json attrs) {
    for (auto &el : attrs.items()) {
        if (el.key() == "public" && el.value().is_boolean()) {
            this->setBoolAttribute("public", el.value());
            continue;
        }

        if (el.key() == "mtime" && el.value().is_number_integer()) {
            this->setIntAttribute("mtime", el.value());
            continue;
        }

        throw InvalidArgsException("Invalid attribute " + el.key());
    }
}

json Database::getAttributes() const {
    json j;

    j["public"] = this->isPublic();
    j["mtime"] = this->getLastUpdate();

    // See if we have a LICENSE.md and README.md in the index
    {
        const std::string sql =
            "SELECT path FROM entries WHERE path = 'LICENSE.md' OR path = "
            "'README.md'";

        const auto q = this->query(sql);
        while (q->fetch()) {
            const std::string p = q->getText(0);
            if (p == "README.md") {
                j["readme"] = p;
            } else if (p == "LICENSE.md") {
                j["license"] = p;
            }
        }
    }

    // Count entries
    {
        const std::string sql = "SELECT COUNT(1) FROM entries";
        const auto q = this->query(sql);
        while (q->fetch()) {
            j["entries"] = q->getInt(0);
        }
    }
    
    return j;
}

fs::path Database::rootDirectory() const{
   return fs::path(this->getOpenFile()).parent_path().parent_path();
}

void Database::setBoolAttribute(const std::string &name, bool value) {
    this->setIntAttribute(name, value ? 1 : 0);
}

bool Database::getBoolAttribute(const std::string &name) const {
    return this->getIntAttribute(name) == 1;
}

void Database::setLongAttribute(const std::string &name, long long value) {
    const std::string sql =
        "INSERT OR REPLACE INTO attributes (name, ivalue) "
        "VALUES(?, ?)";

    const auto q = this->query(sql);

    q->bind(1, name);
    q->bind(2, value);

    q->execute();
}

void Database::setIntAttribute(const std::string &name, long value) {
    const std::string sql =
        "INSERT OR REPLACE INTO attributes (name, ivalue) "
        "VALUES(?, ?)";

    const auto q = this->query(sql);

    q->bind(1, name);
    q->bind(2, static_cast<long long>(value));

    q->execute();
}

int Database::getIntAttribute(const std::string &name) const {
    const std::string sql = "SELECT ivalue FROM attributes WHERE name = ?";

    const auto q = this->query(sql);
    q->bind(1, name);
    return q->fetch() ? q->getInt(0) : 0;
}

long long Database::getLongAttribute(const std::string &name) const {
    const std::string sql = "SELECT ivalue FROM attributes WHERE name = ?";

    const auto q = this->query(sql);
    q->bind(1, name);
    return q->fetch() ? q->getInt64(0) : 0;
}

bool Database::hasAttribute(const std::string &name) const {
    const std::string sql = "SELECT COUNT(name) FROM attributes WHERE name = ?";

    const auto q = this->query(sql);
    q->bind(1, name);
    q->fetch();

    return q->getInt(0) == 1;
}

void Database::clearAttribute(const std::string &name) {
    const std::string sql = "DELETE FROM attributes WHERE name = ?";

    const auto q = this->query(sql);
    q->bind(1, name);
    q->execute();
}

}  // namespace ddb
