/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DATABASEPOOL_H
#define DATABASEPOOL_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <vector>

#include "ddb_export.h"
#include "fileversion.h"
#include "fs.h"

struct sqlite3;

namespace ddb
{
    class Database;

    /**
     * Process-wide pool of idle index connections, keyed by database file.
     *
     * ddb::open() takes a connection from here when one is idle for its index (skipping
     * the SQLite open, the SpatiaLite setup, the schema parse and the schema checks) and
     * a Database opened by ddb::open() gives its connection back when it is closed or
     * destroyed. A connection is only reused when it is back in autocommit mode with no
     * statement left open, and when the database file is still the same file (device and
     * inode on POSIX). A connection does not checkpoint when it becomes idle: committed
     * changes can stay in the -wal file between calls (until SQLite's automatic checkpoint
     * or the last close), so a copy of the database file alone needs checkpointIndex()
     * (DDBCheckpoint) first; a copy of the whole .ddb folder carries the WAL with it. The
     * pool is never destroyed (its connections must not close during static destruction),
     * so at a normal process exit it checkpoints its idle connections instead: after the
     * process the database file alone holds the data, as after a last close.
     *
     * Replacing or deleting an index file of this process while its connections are idle
     * requires releaseConnections() first (DDBReleaseConnections in the C API): SQLite does
     * not support changing a database file under an open connection, and on Windows open
     * files cannot be deleted.
     *
     * Configuration (read once, from the environment):
     *   DDB_DB_POOL_SIZE          idle connections kept per index (default 4, 0 disables)
     *   DDB_DB_POOL_MAX           idle connections kept in total (default 64)
     *   DDB_DB_POOL_IDLE_SECONDS  idle connections are closed after this (default 60)
     */
    class DatabasePool
    {
    public:
        struct Options
        {
            size_t perIndex = 4;
            size_t total = 64;
            std::chrono::seconds idleTimeout{60};
        };

        struct Stats
        {
            size_t idle = 0;
            uint64_t opened = 0;   // connections created by ddb::open()
            uint64_t reused = 0;   // ddb::open() calls served by an idle connection
            uint64_t discarded = 0; // connections closed instead of kept
        };

        DDB_DLL static DatabasePool &instance();

        /// Replaces the options; closes idle connections beyond the new limits
        DDB_DLL void configure(const Options &options);
        DDB_DLL Options options() const;
        DDB_DLL bool enabled() const;

        /// Gives db an idle connection to dbFile. False when none is available.
        DDB_DLL bool acquire(const fs::path &dbFile, Database &db);

        /// Marks a connection that db has just opened to dbFile (and checked) as one to
        /// give back on close. No effect when the pool is disabled.
        DDB_DLL void adopt(const fs::path &dbFile, Database &db);

        /// Takes the connection of db (opened by ddb::open()): keeps it idle or closes it.
        DDB_DLL void release(Database &db);

        /// Closes the idle connections of every index file under path (a dataset folder,
        /// an organization folder or an index file). Returns how many were closed.
        DDB_DLL size_t releaseConnections(const fs::path &path);

        /// Closes all idle connections
        DDB_DLL size_t clear();

        /// Moves the WAL of every idle connection into its database file (TRUNCATE
        /// checkpoint). Runs at process exit; skipped while another thread holds the pool.
        DDB_DLL void checkpointIdle();

        DDB_DLL Stats stats() const;

    private:
        struct Idle
        {
            std::string key;
            sqlite3 *handle = nullptr;
            void *spatialiteCache = nullptr;
            std::string fileId;
            std::chrono::steady_clock::time_point since;
        };

        DatabasePool();

        void closeAll(std::vector<Idle> &connections);
        // Under lock: moves expired and over-limit connections to out
        void collect(std::vector<Idle> &out, std::chrono::steady_clock::time_point now);

        mutable std::mutex mutex_;
        Options options_;
        std::list<Idle> idle_; // most recently released first
        Stats stats_;
        std::chrono::steady_clock::time_point lastPurge_;
    };


}

#endif // DATABASEPOOL_H
