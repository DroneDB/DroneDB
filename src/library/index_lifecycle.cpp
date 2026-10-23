/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// The index lifecycle: opening an index (through the connection pool), creating one from the
// empty-index template, and checkpointing its WAL.
#include "dbops.h"
#include "randomutil.h"

#include "databasepool.h"
#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "userprofile.h"
#include "version.h"
#include "constants.h"

#include <system_error>

namespace ddb
{

namespace
{
    // Removes a closed SQLite database and the -wal/-shm files SQLite keeps next to it
    // (Database::afterOpen sets SQLITE_FCNTL_PERSIST_WAL). Best effort.
    void removeDatabaseFiles(const fs::path &file, bool keepMainFile = false)
    {
        std::error_code ignored;
        if (!keepMainFile)
            fs::remove(file, ignored);
        fs::remove(io::concat(file, "-wal"), ignored);
        fs::remove(io::concat(file, "-shm"), ignored);
    }

    // A database file under construction: removed with its -wal/-shm files when the owner
    // goes out of scope, unless it was published (renamed into place) first
    class TemporaryDatabaseFile
    {
        fs::path file_;
        bool published_ = false;

      public:
        explicit TemporaryDatabaseFile(fs::path file) : file_(std::move(file)) {}
        ~TemporaryDatabaseFile()
        {
            if (!published_)
                removeDatabaseFiles(file_);
        }
        TemporaryDatabaseFile(const TemporaryDatabaseFile &) = delete;
        TemporaryDatabaseFile &operator=(const TemporaryDatabaseFile &) = delete;

        const fs::path &path() const { return file_; }

        // Renames the closed database to target (its -wal/-shm files are removed). False when
        // the rename fails: the file stays owned and is removed on scope exit.
        bool publish(const fs::path &target, std::error_code &error)
        {
            // Closed by its only connection: checkpointed, the WAL is empty
            removeDatabaseFiles(file_, true);
            fs::rename(file_, target, error);
            published_ = !error;
            return published_;
        }
    };

    // Creates the empty index template that initIndex() copies, unless it exists.
    //
    // Threads and processes sharing a profile can get here together on a fresh profile. Each
    // builds the template in a temporary file of its own, closes it and renames it to
    // templatePath: the rename is atomic, so templatePath only ever names a complete
    // database. The last rename wins on POSIX (all candidates are identical); where the
    // rename does not replace an existing file (Windows), the caller that loses discards
    // its copy.
    void ensureEmptyIndexTemplate(const fs::path &templatePath)
    {
        if (exists(templatePath))
            return;

        LOGD << "Creating " << io::toUtf8(templatePath);
        TemporaryDatabaseFile temporary(io::concat(templatePath, ".tmp-" + randomString(16)));
        {
            Database db;
            db.open(io::toUtf8(temporary.path()));
            db.createTables();
            db.close();
        }

        std::error_code e;
        if (!temporary.publish(templatePath, e))
        {
            if (!exists(templatePath))
                throw FSException("Cannot publish " + io::toUtf8(templatePath) + " (" +
                                  e.message() + ")", e);
            LOGD << io::toUtf8(templatePath) << " was published by another process";
        }
    }
}

    std::unique_ptr<Database> open(const std::string &directory,
                                   bool traverseUp = false)
    {
        const fs::path dirPath = fs::absolute(io::fromUtf8(directory));
        const fs::path ddbDirPath = dirPath / DDB_FOLDER;
        const fs::path dbasePath = ddbDirPath / DDB_DATABASE_FILE;

        if (!exists(dbasePath))
        {
            if (!traverseUp || dirPath.parent_path() == dirPath)
                throw FSException(
                    "Not a valid DroneDB directory, .ddb does not exist. Did you "
                    "run ddb init?");

            return open(io::toUtf8(dirPath.parent_path()), true);
        }

        LOGD << io::toUtf8(dbasePath) + " exists";

        auto db = std::make_unique<Database>();

        // An idle connection to this index skips the open, the SpatiaLite setup, the
        // schema parse and the checks below (it passed them when it was opened)
        auto &pool = DatabasePool::instance();
        if (pool.acquire(dbasePath, *db))
            return db;

        db->open(io::toUtf8(dbasePath));

        if (!db->tableExists("entries"))
            throw DBException("Table 'entries' not found (not a valid database: " +
                              io::toUtf8(dbasePath) + ")");

        db->ensureSchemaConsistency();
        db->removeRootEntry();
        pool.adopt(dbasePath, *db);

        return db;
    }

    bool checkpointIndex(const std::string &directory)
    {
        const auto db = ddb::open(directory, false);
        // One row: busy (1 when blocked), WAL frames, frames checkpointed
        const auto q = db->query("PRAGMA wal_checkpoint(TRUNCATE)");
        return !q->fetch() || q->getInt(0) == 0;
    }

    std::string initIndex(const std::string &directory, bool fromScratch)
    {
        const fs::path dirPath = io::fromUtf8(directory);
        if (!exists(dirPath))
            throw FSException("Invalid directory: " + io::toUtf8(dirPath) +
                              " (does not exist)");

        auto ddbDirPath = dirPath / DDB_FOLDER;
        if (std::string(directory) == ".")
            ddbDirPath = DDB_FOLDER; // Nicer to the eye
        const auto dbasePath = ddbDirPath / DDB_DATABASE_FILE;

        LOGD << "Checking if .ddb directory exists...";
        if (exists(ddbDirPath))
        {
            throw FSException("Cannot initialize database: " + io::toUtf8(ddbDirPath) +
                              " already exists");
        }

        if (create_directory(ddbDirPath))
        {
            LOGD << io::toUtf8(ddbDirPath) + " created";
        }
        else
        {
            throw FSException("Cannot create directory: " + io::toUtf8(ddbDirPath) +
                              ". Check that you have the proper permissions?");
        }

        LOGD << "Checking if database exists...";
        if (exists(dbasePath))
        {
            throw FSException(io::toUtf8(ddbDirPath) + " already exists");
        }

        if (!fromScratch)
        {
            // "Fast" init by copying the pre-built empty database index
            // this prevents the slow table generation process
            const fs::path emptyDbPath = UserProfile::get()->getTemplatesDir() /
                                         (std::string("empty-dbase-") + ddb::revision() + ".sqlite");

            ensureEmptyIndexTemplate(emptyDbPath);

            if (exists(emptyDbPath))
            {
                // Copy
                try
                {
                    copy(emptyDbPath, dbasePath);
                }
                catch (fs::filesystem_error &e)
                {
                    throw FSException(e.what());
                }

                LOGD << "Copied " << io::toUtf8(emptyDbPath) << " to "
                     << io::toUtf8(dbasePath);
            }
            else
            {
                // For some reason it's missing, generate from scratch
                LOGD << "Cannot find " << io::toUtf8(emptyDbPath)
                     << " in data path, strange! Building from scratch instead";
                fromScratch = true;
            }
        }

        if (fromScratch)
        {
            LOGD << "Creating " << io::toUtf8(dbasePath);

            // Create database
            Database db;
            db.open(io::toUtf8(dbasePath));
            db.createTables();
            db.close();
        }

        return io::toUtf8(ddbDirPath);
    }

} // namespace ddb
