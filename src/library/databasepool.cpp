/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "databasepool.h"

#include <algorithm>
#include <cstdlib>

#include "database.h"
#include "fileversion.h"
#include "logger.h"

namespace ddb
{
    namespace
    {
        size_t envSize(const char *name, size_t fallback)
        {
            const char *value = std::getenv(name);
            if (value == nullptr || *value == '\0')
                return fallback;
            try
            {
                return static_cast<size_t>(std::stoul(value));
            }
            catch (const std::exception &)
            {
                LOGW << "Ignoring invalid " << name << "=" << value;
                return fallback;
            }
        }

        // Idle connections are closed at most this often when they expire
        constexpr std::chrono::seconds PurgeInterval{5};

        std::string poolKey(const fs::path &dbFile)
        {
            return cacheKeyPath(dbFile);
        }

        void closeHandle(sqlite3 *handle, void *spatialiteCache)
        {
            // The SpatiaLite cache must outlive the connection that uses it
            if (handle != nullptr)
                sqlite3_close_v2(handle);
            freeSpatialiteCache(spatialiteCache);
        }
    }

    DatabasePool &DatabasePool::instance()
    {
        // Never destroyed: connections must not be closed during static destruction,
        // after SQLite, PROJ or GEOS state may be gone. The OS closes them at exit.
        static auto *pool = new DatabasePool();
        return *pool;
    }

    DatabasePool::DatabasePool() : lastPurge_(std::chrono::steady_clock::now())
    {
        options_.perIndex = envSize("DDB_DB_POOL_SIZE", options_.perIndex);
        options_.total = envSize("DDB_DB_POOL_MAX", options_.total);
        options_.idleTimeout = std::chrono::seconds(
            envSize("DDB_DB_POOL_IDLE_SECONDS", static_cast<size_t>(options_.idleTimeout.count())));

        // Idle connections keep committed changes in the WAL and are never closed (the pool
        // outlives static destruction): empty the WAL at exit, so that no host leaves frames
        // that a later process would apply to a database file put in its place
        std::atexit([] { DatabasePool::instance().checkpointIdle(); });
    }

    void DatabasePool::checkpointIdle()
    {
        // A thread inside the pool at exit keeps its lock: skip rather than wait forever
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock())
            return;
        // SQLite only (no logging): other static objects may be gone by now
        for (const auto &c : idle_)
            sqlite3_wal_checkpoint_v2(c.handle, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr);
    }

    size_t DatabasePool::shutdown()
    {
        std::vector<Idle> closing;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto &c : idle_)
            {
                const int rc = sqlite3_wal_checkpoint_v2(c.handle, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr);
                if (rc != SQLITE_OK)
                    LOGW << "Cannot checkpoint " << c.key << " at shutdown (" << sqlite3_errstr(rc)
                         << "): its last close checkpoints it";
            }
            closing.assign(idle_.begin(), idle_.end());
            idle_.clear();
            stats_.discarded += closing.size();
        }
        const auto count = closing.size();
        closeAll(closing);
        return count;
    }

    void DatabasePool::configure(const Options &options)
    {
        std::vector<Idle> stale;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            options_ = options;
            collect(stale, std::chrono::steady_clock::now());
        }
        closeAll(stale);
    }

    DatabasePool::Options DatabasePool::options() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return options_;
    }

    bool DatabasePool::enabled() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return options_.perIndex > 0 && options_.total > 0;
    }

    bool DatabasePool::acquire(const fs::path &dbFile, Database &db)
    {
        const auto key = poolKey(dbFile);
        const auto fileId = fileIdentity(dbFile);
        const auto now = std::chrono::steady_clock::now();

        std::vector<Idle> stale;
        Idle found;
        bool hit = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (options_.perIndex == 0 || options_.total == 0)
                return false;

            collect(stale, now);

            for (auto it = idle_.begin(); it != idle_.end();)
            {
                if (it->key != key)
                {
                    ++it;
                    continue;
                }

                if (it->fileId != fileId)
                {
                    // The path now names another file (deleted and recreated, replaced)
                    stale.push_back(*it);
                    stats_.discarded++;
                    it = idle_.erase(it);
                    continue;
                }

                found = *it;
                idle_.erase(it);
                hit = true;
                stats_.reused++;
                break;
            }
        }
        closeAll(stale);

        if (!hit)
            return false;

        db.attachPooled(found.handle, found.spatialiteCache, dbFile.string(), key, found.fileId);
        return true;
    }

    void DatabasePool::adopt(const fs::path &dbFile, Database &db)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (options_.perIndex == 0 || options_.total == 0)
                return;
            stats_.opened++;
        }
        db.markPooled(poolKey(dbFile), fileIdentity(dbFile));
    }

    void DatabasePool::release(Database &db)
    {
        Idle connection;
        connection.handle = db.detachPooled(connection.spatialiteCache, connection.key, connection.fileId);
        if (connection.handle == nullptr)
            return;

        // Reusable only in a clean state: a transaction left open or a statement still
        // alive (its owner could step it later from another thread) closes it.
        bool keep = sqlite3_get_autocommit(connection.handle) != 0 &&
                    sqlite3_next_stmt(connection.handle, nullptr) == nullptr;

        // No checkpoint here: a checkpoint syncs the WAL and the database file (two or three
        // fsyncs, 8-15 ms on an SSD, more on disks), and every index write would pay it.
        // Committed changes stay in the WAL until SQLite's automatic checkpoint (every ~1000
        // pages), the last connection's close, or checkpointIndex() (DDBCheckpoint), which
        // whoever copies the database file without its WAL must call first.

        std::vector<Idle> stale;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (options_.perIndex == 0 || options_.total == 0)
                keep = false;

            if (keep)
            {
                connection.since = std::chrono::steady_clock::now();
                idle_.push_front(connection);
                collect(stale, connection.since);
            }
            else
            {
                stats_.discarded++;
            }
        }

        if (!keep)
            closeHandle(connection.handle, connection.spatialiteCache);
        closeAll(stale);
    }

    size_t DatabasePool::releaseConnections(const fs::path &path)
    {
        const auto prefix = poolKey(path);
        std::vector<Idle> closing;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = idle_.begin(); it != idle_.end();)
            {
                if (isCacheKeyUnder(it->key, prefix))
                {
                    closing.push_back(*it);
                    it = idle_.erase(it);
                }
                else
                    ++it;
            }
            stats_.discarded += closing.size();
        }
        const auto count = closing.size();
        closeAll(closing);
        return count;
    }

    size_t DatabasePool::clear()
    {
        std::vector<Idle> closing;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closing.assign(idle_.begin(), idle_.end());
            idle_.clear();
            stats_.discarded += closing.size();
        }
        const auto count = closing.size();
        closeAll(closing);
        return count;
    }

    DatabasePool::Stats DatabasePool::stats() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto s = stats_;
        s.idle = idle_.size();
        return s;
    }

    void DatabasePool::collect(std::vector<Idle> &out, std::chrono::steady_clock::time_point now)
    {
        const size_t before = out.size();

        // Expired (checked every PurgeInterval, or more often for a shorter timeout)
        if (now - lastPurge_ >= std::min<std::chrono::steady_clock::duration>(PurgeInterval, options_.idleTimeout))
        {
            lastPurge_ = now;
            for (auto it = idle_.begin(); it != idle_.end();)
            {
                if (now - it->since >= options_.idleTimeout)
                {
                    out.push_back(*it);
                    it = idle_.erase(it);
                }
                else
                    ++it;
            }
        }

        // Per index: keep the most recently released
        if (options_.perIndex > 0)
        {
            std::vector<std::pair<std::string, size_t>> counts;
            for (auto it = idle_.begin(); it != idle_.end();)
            {
                auto c = std::find_if(counts.begin(), counts.end(),
                                      [&](const auto &p) { return p.first == it->key; });
                if (c == counts.end())
                {
                    counts.emplace_back(it->key, 1);
                    ++it;
                }
                else if (++c->second > options_.perIndex)
                {
                    out.push_back(*it);
                    it = idle_.erase(it);
                }
                else
                    ++it;
            }
        }

        // In total: drop the least recently released
        while (idle_.size() > options_.total)
        {
            out.push_back(idle_.back());
            idle_.pop_back();
        }

        stats_.discarded += out.size() - before;
    }

    void DatabasePool::closeAll(std::vector<Idle> &connections)
    {
        for (auto &c : connections)
            closeHandle(c.handle, c.spatialiteCache);
        connections.clear();
    }

}
