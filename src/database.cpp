/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "database.h"
#include "metamanager.h"

#include <fstream>
#include <string>

#include "exceptions.h"
#include "hash.h"
#include "logger.h"
#include "mio.h"

namespace ddb {

void Database::afterOpen() {
    spatialiteCache = spatialite_alloc_connection();
    spatialite_init_ex(db, spatialiteCache, 0);
    
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
                            passwordsTableDdl;

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

    // Migration from 1.0.7 --> 1.0.8 (can be removed in the near future)
    // we removed the attributes table (use entries_meta instead)
    // TODO: remove me in 2023
    if (this->tableExists("attributes")){
        // Port public attr info to visibility meta
        {
            const auto q = this->query("SELECT ivalue FROM attributes WHERE name = 'public'");
            if (q->fetch()){
                if (q->getInt(0) == 1){
                    this->getMetaManager()->set("visibility", "1");
                    LOGD << "Migrated attributes.public to entries_meta.visibility";
                }
            }
        }

        // Drop table
        this->exec("DROP TABLE attributes");
        LOGD << "Dropped attributes table";
    }

}

json Database::getProperties() const {
    json j;

    // See if we have a README.md in the index
    {
        const std::string sql =
            "SELECT path FROM entries WHERE path = "
            "'README.md'";

        const auto q = this->query(sql);
        while (q->fetch()) {
            const std::string p = q->getText(0);
            if (p == "README.md") {
                j["readme"] = p;
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

    // Find meta
    {
        const std::string sql = R"<<<(SELECT CASE
                WHEN key IS NULL THEN NULL
                ELSE json_group_object(key, meta)
            END AS meta
                    FROM (
                        SELECT key, CASE WHEN substr(key, -1, 1) = 's'
                            THEN json_group_array(json_object('id', emi.id, 'data', json(emi.data), 'mtime', emi.mtime))
                            ELSE json_object('id', emi.id, 'data', json(emi.data), 'mtime', emi.mtime)
                        END AS meta
                        FROM entries_meta emi
                        WHERE path = ""
                        GROUP BY key
                    )
            )<<<";
        const auto q = this->query(sql);
        if (q->fetch()){
            const std::string metaJson = q->getText(0);
            if (!metaJson.empty()){
                try{
                    json meta = json::parse(metaJson);
                    if (!meta.empty()) j["meta"] = meta;
                }catch(json::exception){
                    LOGD << "Malformed database meta: " << metaJson;
                }
            }
        }
    }
    
    return j;
}

json Database::getStamp() const{
    json j;
    SHA256 checksum;

    auto q = this->query("SELECT path,hash FROM entries ORDER BY path ASC");
    j["entries"] = json::array();
    while (q->fetch()){
        const std::string p = q->getText(0);
        const std::string h = q->getText(1);
        checksum.add(p.c_str(), p.length());
        checksum.add(h.c_str(), h.length());

        j["entries"].push_back(json::object({{p, h}}));
    }

    q = this->query("SELECT id FROM entries_meta ORDER BY id ASC");
    j["meta"] = json::array();
    while (q->fetch()){
        const std::string id = q->getText(0);
        checksum.add(id.c_str(), id.length());
        j["meta"].push_back(id);
    }

    j["checksum"] = checksum.getHash();
    return j;
}

fs::path Database::rootDirectory() const{
    return fs::path(this->getOpenFile()).parent_path().parent_path();
}

fs::path Database::ddbDirectory() const{
    return fs::path(this->getOpenFile()).parent_path();
}

fs::path Database::buildDirectory() const{
    return ddbDirectory() / DDB_BUILD_PATH;
}

std::string Database::getReadme() const{
    const std::string sql = "SELECT path FROM entries WHERE path = 'README.md'";

    const auto q = this->query(sql);
    if (q->fetch()) {
        const std::string p = q->getText(0);
        if (p == "README.md") {
            std::ifstream f((this->rootDirectory() / p).string());
            if (f.is_open()){
                std::stringstream buffer;
                buffer << f.rdbuf();
                return buffer.str();
            }
        }
    }
    return "";
}

json Database::getExtent() const{
    json j;
    json j_null;

    const auto sql = "SELECT AsWKT(Extent(GUnion(polygon_geom, ConvexHull(point_geom)))) AS bbox FROM entries";
    const auto q = this->query(sql);
    if (q->fetch()){
        const auto bbox = wktBboxCoordinates(q->getText(0));
        if (bbox.size() > 0){
            j["spatial"] = {{"bbox", json::array({bbox}) }};
        }
    }

    if (!j.contains("spatial")){
        j["spatial"] = {{"bbox", json::array({
                                   json::array({
                                       0, 0, 0, 0, 0, 0
                                   })
                               })
                        }};
    }

    // No reliable temporal information is available
    // TODO: some assets (geoimages) have temporal information
    // perhaps there might be use in the future to populate it here
    // Other ideas include using creation date / modification date, but
    // these do not reflect the actual time of the assets
    j["temporal"] = {{"interval", json::array({
                                      json::array({j_null, j_null})
                                  })
                    }};

    return j;
}

MetaManager *Database::getMetaManager(){
    if (metaManager == nullptr){
        metaManager = new MetaManager(this);
    }
    return metaManager;
}

Database::~Database(){
    if (metaManager != nullptr){
        delete metaManager;
        metaManager = nullptr;
    }
    if (spatialiteCache != nullptr){
        spatialite_cleanup_ex(spatialiteCache);
        spatialiteCache = nullptr;
    }
}

json wktBboxCoordinates(const std::string &wktBbox){
    std::vector<double> values;
    std::string bbox = wktBbox;

    if (!bbox.empty() && bbox.size() > 9){
        // Extract bbox coordinates

        bbox.erase(0, std::string("POLYGON((").size());
        std::istringstream ss(bbox);

        double d;
        char c;
        int i = 0;

        while(ss >> d){
            values.push_back(d);
            if (++i % 2 == 0) ss >> c; // skip ','
        }
    }

    json ret = json::array();

    if (values.size() == 10){
        ret.push_back(values[0]);
        ret.push_back(values[1]);
        ret.push_back(values[4]);
        ret.push_back(values[5]);
    }

    return ret;
}

}  // namespace ddb
