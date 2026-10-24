/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "database.h"
#include "index_sql.h"
#include "json.h"
#include "metamanager.h"
#include "databasepool.h"

#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>

#include "exceptions.h"
#include "hash.h"
#include "logger.h"
#include "mio.h"

namespace ddb
{

    void freeSpatialiteCache(void *cache)
    {
        if (cache == nullptr)
            return;

        // Upstream spatialite_cleanup_ex() is not per connection only: it also runs
        // xmlCleanupParser(), the process-wide libxml2 teardown, which is not thread-safe. Two
        // caches freed at once tore libxml2 down together (one thread destroyed a libxml2 mutex
        // another was waiting for, which then blocked forever), and per-thread libxml2 state
        // leaked. Our libspatialite overlay (vcpkg-overlays/libspatialite) removes that call;
        // the caches are still freed one at a time, so a libspatialite built without the
        // overlay cannot deadlock. Connections close on any thread (concurrent C API calls,
        // DatabasePool). Never destroyed (like DatabasePool): a connection may still close
        // during static destruction.
        static auto *cleanupMutex = new std::mutex();
        std::lock_guard<std::mutex> lock(*cleanupMutex);
        spatialite_cleanup_ex(cache);
    }

    void Database::afterOpen()
    {
        // spatialite_initialize() sets an unguarded global flag (and initializes libxml2) on
        // its first call, which spatialite_alloc_connection() makes implicitly: run it once.
        // With a reentrant GEOS build (GEOS_REENTRANT) the rest of the per-connection setup
        // only touches the new cache and this handle, so connections can open concurrently
        // (freeing a cache cannot: see freeSpatialiteCache).
        static std::once_flag spatialiteInitialized;
        std::call_once(spatialiteInitialized, [] { spatialite_initialize(); });

        // reopen() runs afterOpen() again on a closed handle
        freeSpatialiteCache(spatialiteCache);
        spatialiteCache = spatialite_alloc_connection();
        spatialite_init_ex(db, spatialiteCache, 0);

        // Busy timeout after spatialite_init_ex(), which sets its own (5 s), and before any
        // statement: journal-mode conversion needs to acquire an exclusive lock and must be
        // able to wait for it. Kept small (busyTimeoutMs): transaction-acquisition retry/jitter
        // is owned by RetryPolicy, not by this floor — a
        // larger floor here would reintroduce the unjittered wait RetryPolicy is meant to remove.
        // This is only a floor for statements executed outside an explicit Transaction.
        if (sqlite3_busy_timeout(db, busyTimeoutMs) != SQLITE_OK)
        {
            LOGD << "Cannot set busy timeout";
        }

        // Indexes are created in WAL mode: only legacy databases need the switch (which
        // takes an exclusive lock)
        if (this->getJournalMode() != "wal")
            this->setJournalMode("wal");

        // Keep the -wal/-shm files when the last connection closes, so that every
        // read-only call does not delete and recreate them. The size limit truncates the
        // WAL to zero bytes once it has been checkpointed (on close and on reset), so no
        // checkpointed frames are left next to the database file.
        int persistWal = 1;
        sqlite3_file_control(db, "main", SQLITE_FCNTL_PERSIST_WAL, &persistWal);
        this->exec("PRAGMA journal_size_limit=0;");

        this->exec("PRAGMA synchronous=NORMAL;");
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

  CREATE INDEX IF NOT EXISTS ix_entries_hash
  ON entries (hash);
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
  DROP INDEX IF EXISTS ix_entries_meta_path;
  CREATE INDEX IF NOT EXISTS ix_entries_meta_path_key
  ON entries_meta (path, key);
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

    Database &Database::createTables()
    {
        const std::string sql = std::string(entriesTableDdl) + '\n' +
                                passwordsTableDdl + '\n' +
                                entriesMetaTableDdl;

        LOGD << "About to create tables...";
        this->exec(sql);
        LOGD << "Created tables";

        // Set schema version for new databases
        this->setUserVersion(DDB_SCHEMA_VERSION);
        LOGD << "Set schema version to " << DDB_SCHEMA_VERSION;

        return *this;
    }

    DDB_DLL void Database::ensureSchemaConsistency()
    {
        LOGD << "Ensuring schema consistency";

        const int dbVersion = this->getUserVersion();
        LOGD << "Database schema version: " << dbVersion;

        // Check if database was created with a newer version of DroneDB
        if (dbVersion > DDB_SCHEMA_VERSION)
        {
            throw DBException("Database schema version " + std::to_string(dbVersion) +
                " is newer than supported version " + std::to_string(DDB_SCHEMA_VERSION) +
                ". Please update DroneDB to open this database.");
        }

        // Database is already at current version, no migration needed
        if (dbVersion == DDB_SCHEMA_VERSION)
        {
            LOGD << "Database schema is up to date";
            return;
        }

        // Legacy database (version 0) - run all table checks and migrations
        // This handles databases created before the versioning system was introduced
        if (dbVersion == 0)
        {
            LOGD << "Legacy database detected, running full schema check and migrations";

            if (!this->tableExists("entries"))
            {
                LOGD << "Entries table does not exist, creating it";
                this->exec(entriesTableDdl);
                LOGD << "Entries table created";
            }

            if (!this->tableExists("passwords"))
            {
                LOGD << "Passwords table does not exist, creating it";
                this->exec(passwordsTableDdl);
                LOGD << "Passwords table created";
            }

            if (!this->tableExists("entries_meta"))
            {
                LOGD << "Entries meta table does not exist, creating it";
                this->exec(entriesMetaTableDdl);
                LOGD << "Entries meta table created";
            }

            // Ensure indexes exist on existing databases (migration for new indexes)
            // These are idempotent operations that will not fail if indexes already exist
            LOGD << "Ensuring indexes exist";
            this->exec("CREATE INDEX IF NOT EXISTS ix_entries_hash ON entries (hash)");
            this->exec("DROP INDEX IF EXISTS ix_entries_meta_path");
            this->exec("CREATE INDEX IF NOT EXISTS ix_entries_meta_path_key ON entries_meta (path, key)");
            this->exec("CREATE INDEX IF NOT EXISTS ix_entries_meta_key ON entries_meta (key)");

            // Migration from 0.9.11 to 0.9.12
            // Renamed "entries.meta" --> "entries.properties"
            if (this->renameColumnIfExists("entries", "meta TEXT", "properties TEXT"))
            {
                this->reopen();
            }

            // Migration from 1.0.7 --> 1.0.8
            // Removed the attributes table (use entries_meta instead)
            if (this->tableExists("attributes"))
            {
                // Port public attr info to visibility meta
                {
                    const auto q = this->query("SELECT ivalue FROM attributes WHERE name = 'public'");
                    if (q->fetch())
                    {
                        if (q->getInt(0) == 1)
                        {
                            this->getMetaManager()->set("visibility", "1");
                            LOGD << "Migrated attributes.public to entries_meta.visibility";
                        }
                    }
                }

                // Drop table
                this->exec("DROP TABLE attributes");
                LOGD << "Dropped attributes table";
            }

            // Mark database as migrated to current version. A read-only connection (read-only
            // file or filesystem) got here only because the schema needed no change: it
            // cannot record the version, so it leaves it for the next writable open.
            if (this->isReadOnly())
            {
                LOGD << "Read-only database, schema version left at " << dbVersion;
            }
            else
            {
                this->setUserVersion(DDB_SCHEMA_VERSION);
                LOGD << "Database migrated to schema version " << DDB_SCHEMA_VERSION;
            }
        }

        // Future migrations would be added here:
        // if (dbVersion < 2) { runMigration_1_to_2(); }
        // if (dbVersion < 3) { runMigration_2_to_3(); }
        // etc.
    }

    void Database::removeRootEntry()
    {
        try
        {
            if (!this->query("SELECT 1 FROM entries WHERE path = ''")->fetch())
                return;

            this->exec("DELETE FROM entries WHERE path = ''");
            LOGD << "Removed the root entry of " << this->getOpenFile();
        }
        catch (const std::exception &e)
        {
            LOGD << "Cannot remove the root entry of " << this->getOpenFile() << ": " << e.what();
        }
    }

    json Database::getProperties() const
    {
        json j;

        // See if we have a README.md in the index
        {
            const std::string sql =
                "SELECT path FROM entries WHERE path = "
                "'README.md'";

            const auto q = this->query(sql);
            while (q->fetch())
            {
                const std::string p = q->getText(0);
                if (p == "README.md")
                {
                    j["readme"] = p;
                }
            }
        }

        // Count entries
        {
            const std::string sql = "SELECT COUNT(1) FROM entries";
            const auto q = this->query(sql);
            while (q->fetch())
            {
                j["entries"] = q->getInt(0);
            }
        }

        // Find meta
        {
            // The metadata of the dataset: the entries_meta rows with the empty path
            const std::string sql = metaAggregateSql("''");
            const auto q = this->query(sql);
            if (q->fetch())
            {
                const std::string metaJson = q->getText(0);
                if (!metaJson.empty())
                {
                    try
                    {
                        json meta = json::parse(metaJson);
                        if (!meta.empty())
                            j["meta"] = meta;
                    }
                    catch (json::exception)
                    {
                        LOGD << "Malformed database meta: " << metaJson;
                    }
                }
            }
        }

        return j;
    }

    namespace
    {
        // Appends `s` as a JSON string literal, escaped like json::dump() does.
        void appendJsonString(std::string &out, const std::string &s)
        {
            static const char *hex = "0123456789abcdef";
            out += '"';
            size_t plainStart = 0;
            for (size_t i = 0; i < s.size(); i++)
            {
                const auto c = static_cast<unsigned char>(s[i]);
                if (c >= 0x20 && c != '"' && c != '\\')
                    continue;

                out.append(s, plainStart, i - plainStart);
                plainStart = i + 1;
                switch (c)
                {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    out += "\\u00";
                    out += hex[c >> 4];
                    out += hex[c & 0x0f];
                }
            }
            out.append(s, plainStart, std::string::npos);
            out += '"';
        }
    }

    std::string Database::readStamp(const StampEntryVisitor &onEntry, const StampMetaVisitor &onMeta) const
    {
        // The checksum is a SHA-256 over every path+hash (path order), then every meta id
        // (id order). Clients compare it across versions: keep the input and order fixed.
        Sha256 checksum;

        auto q = this->query("SELECT path,hash FROM entries ORDER BY path ASC");
        while (q->fetch())
        {
            const std::string p = q->getText(0);
            const std::string h = q->getText(1);
            checksum.update(p);
            checksum.update(h);
            if (onEntry) onEntry(p, h);
        }

        q = this->query("SELECT id FROM entries_meta ORDER BY id ASC");
        while (q->fetch())
        {
            const std::string id = q->getText(0);
            checksum.update(id);
            if (onMeta) onMeta(id);
        }

        return checksum.finishHex();
    }

    json Database::emptyStamp()
    {
        // The checksum of readStamp over no entry and no metadata
        return json{{"entries", json::array()}, {"meta", json::array()}, {"checksum", Sha256().finishHex()}};
    }

    json Database::getStamp() const
    {
        json j;
        j["entries"] = json::array();
        j["meta"] = json::array();
        j["checksum"] = readStamp(
            [&j](const std::string &p, const std::string &h)
            { j["entries"].push_back(json::object({{p, h}})); },
            [&j](const std::string &id)
            { j["meta"].push_back(id); });
        return j;
    }

    std::string Database::getStampJson() const
    {
        // Same document as getStamp().dump(), written directly: a 50k-entry stamp would
        // otherwise allocate one json object per entry and serialize them afterwards.
        // "checksum" comes first (keys are sorted) but is known last: reserve its place.
        static const std::string head = "{\"checksum\":\"";
        constexpr size_t checksumLength = 64; // hex SHA-256

        std::string out = head + std::string(checksumLength, '0') + "\",\"entries\":[";
        const size_t firstEntry = out.size();
        std::string meta = "[";
        const auto checksum = readStamp(
            [&out, firstEntry](const std::string &p, const std::string &h)
            {
                if (out.size() > firstEntry) out += ',';
                out += '{';
                appendJsonString(out, p);
                out += ':';
                appendJsonString(out, h);
                out += '}';
            },
            [&meta](const std::string &id)
            {
                if (meta.size() > 1) meta += ',';
                appendJsonString(meta, id);
            });

        if (checksum.size() != checksumLength)
            throw AppException("Unexpected stamp checksum length");
        out.replace(head.size(), checksumLength, checksum);

        out += "],\"meta\":";
        out += meta;
        out += "]}";
        return out;
    }

    std::string Database::getStampChecksum() const
    {
        return readStamp(nullptr, nullptr);
    }

    fs::path Database::rootDirectory() const
    {
        return io::fromUtf8(this->getOpenFile()).parent_path().parent_path();
    }

    fs::path Database::ddbDirectory() const
    {
        return io::fromUtf8(this->getOpenFile()).parent_path();
    }

    fs::path Database::buildDirectory() const
    {
        return ddbDirectory() / DDB_BUILD_PATH;
    }

    std::string Database::getReadme() const
    {
        const std::string sql = "SELECT path FROM entries WHERE path = 'README.md'";

        const auto q = this->query(sql);
        if (q->fetch())
        {
            const std::string p = q->getText(0);
            if (p == "README.md")
            {
                std::ifstream f(this->rootDirectory() / io::fromUtf8(p));
                if (f.is_open())
                {
                    std::stringstream buffer;
                    buffer << f.rdbuf();
                    return buffer.str();
                }
            }
        }
        return "";
    }

    json Database::getExtent() const
    {
        json j;
        json j_null;

        const auto sql = "SELECT AsWKT(Extent(GUnion(polygon_geom, ConvexHull(point_geom)))) AS bbox FROM entries";
        const auto q = this->query(sql);
        if (q->fetch())
        {
            const auto bbox = wktBboxCoordinates(q->getText(0));
            if (bbox.size() > 0)
            {
                j["spatial"] = {{"bbox", json::array({bbox})}};
            }
        }

        if (!j.contains("spatial"))
        {
            j["spatial"] = {{"bbox", json::array({json::array({0, 0, 0, 0, 0, 0})})}};
        }

        // Temporal extent: try captureTime from properties, fall back to mtime
        std::string startDatetime;
        std::string endDatetime;

        // First try captureTime (milliseconds UTC epoch) from JSON properties
        {
            const auto tq = this->query(
                "SELECT MIN(json_extract(properties, '$.captureTime')), "
                "MAX(json_extract(properties, '$.captureTime')) "
                "FROM entries WHERE json_extract(properties, '$.captureTime') > 0");
            if (tq->fetch())
            {
                double minCapture = tq->getDouble(0);
                double maxCapture = tq->getDouble(1);
                if (minCapture > 0)
                {
                    time_t minSecs = static_cast<time_t>(minCapture / 1000.0);
                    time_t maxSecs = static_cast<time_t>(maxCapture / 1000.0);
                    struct tm utcTime;
                    char buf[32];
#ifdef _WIN32
                    gmtime_s(&utcTime, &minSecs);
#else
                    gmtime_r(&minSecs, &utcTime);
#endif
                    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utcTime);
                    startDatetime = buf;
#ifdef _WIN32
                    gmtime_s(&utcTime, &maxSecs);
#else
                    gmtime_r(&maxSecs, &utcTime);
#endif
                    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utcTime);
                    endDatetime = buf;
                }
            }
        }

        // Fall back to mtime if no captureTime found
        if (startDatetime.empty())
        {
            const auto tq = this->query(
                "SELECT MIN(mtime), MAX(mtime) FROM entries WHERE mtime > 0");
            if (tq->fetch())
            {
                time_t minMtime = static_cast<time_t>(tq->getInt64(0));
                time_t maxMtime = static_cast<time_t>(tq->getInt64(1));
                if (minMtime > 0)
                {
                    struct tm utcTime;
                    char buf[32];
#ifdef _WIN32
                    gmtime_s(&utcTime, &minMtime);
#else
                    gmtime_r(&minMtime, &utcTime);
#endif
                    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utcTime);
                    startDatetime = buf;
#ifdef _WIN32
                    gmtime_s(&utcTime, &maxMtime);
#else
                    gmtime_r(&maxMtime, &utcTime);
#endif
                    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utcTime);
                    endDatetime = buf;
                }
            }
        }

        if (!startDatetime.empty())
        {
            j["temporal"] = {{"interval", json::array({json::array({startDatetime, endDatetime})})}};
        }
        else
        {
            j["temporal"] = {{"interval", json::array({json::array({j_null, j_null})})}};
        }

        return j;
    }

    MetaManager *Database::getMetaManager()
    {
        if (metaManager == nullptr)
            metaManager = std::make_unique<MetaManager>(this);
        return metaManager.get();
    }

    Database::~Database()
    {
        this->close();
    }

    SqliteDatabase &Database::close()
    {
        if (db != nullptr && pooled)
        {
            DatabasePool::instance().release(*this);
            return *this;
        }

        // The SpatiaLite cache must outlive the connection that uses it
        closeConnection();
        freeSpatialiteCache(spatialiteCache);
        spatialiteCache = nullptr;
        return *this;
    }

    void Database::attachPooled(sqlite3 *handle, void *cache, const std::string &file, const std::string &key,
                                const std::string &fileId)
    {
        if (db != nullptr)
            throw DBException("Can't attach a pooled connection to " + file + ", one is already open (" + openFile + ")");

        db = handle;
        openFile = file;
        spatialiteCache = cache;
        pooled = true;
        poolKey = key;
        poolFileId = fileId;
    }

    void Database::markPooled(const std::string &key, const std::string &fileId)
    {
        pooled = true;
        poolKey = key;
        poolFileId = fileId;
    }

    sqlite3 *Database::detachPooled(void *&cache, std::string &key, std::string &fileId)
    {
        sqlite3 *handle = db;
        if (handle == nullptr)
            return nullptr;

        cache = spatialiteCache;
        key = poolKey;
        fileId = poolFileId;

        db = nullptr;
        spatialiteCache = nullptr;
        pooled = false;
        return handle;
    }

    json wktBboxCoordinates(const std::string &wktBbox)
    {
        std::vector<double> values;
        std::string bbox = wktBbox;

        if (!bbox.empty() && bbox.size() > 9)
        {
            // Extract bbox coordinates

            bbox.erase(0, std::string("POLYGON((").size());
            std::istringstream ss(bbox);

            double d;
            char c;
            int i = 0;

            while (ss >> d)
            {
                values.push_back(d);
                if (++i % 2 == 0)
                    ss >> c; // skip ','
            }
        }

        json ret = json::array();

        if (values.size() == 10)
        {
            ret.push_back(values[0]);
            ret.push_back(values[1]);
            ret.push_back(values[4]);
            ret.push_back(values[5]);
        }

        return ret;
    }

} // namespace ddb
