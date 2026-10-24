/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <cassert>
#include <string>

#include "cancellation.h"
#include "stringutil.h"
#include "logger.h"
#include "exceptions.h"
#include "sqlite_database.h"

namespace ddb
{

    SqliteDatabase::SqliteDatabase() = default;

    SqliteDatabase &SqliteDatabase::open(const std::string &file)
    {
        if (db != nullptr)
            throw DBException("Can't open database " + file + ", one is already open (" + openFile + ")");
        LOGD << "Opening connection to " << file;
        if (sqlite3_open(file.c_str(), &db) != SQLITE_OK)
            throw DBException("Can't open database: " + file);
        // Statements stop when the work of the thread running them is canceled (DQ-15)
        cancel::installSqliteProgressHandler(db);

        this->openFile = file;
        this->afterOpen();

        return *this;
    }

    void SqliteDatabase::afterOpen()
    {
        // Nothing
    }

    SqliteDatabase &SqliteDatabase::close()
    {
        closeConnection();
        return *this;
    }

    void SqliteDatabase::closeConnection()
    {
        if (db != nullptr)
        {
            LOGD << "Closing connection to " << openFile;
            // _v2 marks the connection a zombie instead of leaking it (and its locks) when
            // a statement was left unfinalized; sqlite3_close() would keep the handle alive.
            const int rc = sqlite3_close_v2(db);
            if (rc != SQLITE_OK)
                LOGD << "sqlite3_close_v2 returned " << rc;
            db = nullptr;
        }
    }

    SqliteDatabase &SqliteDatabase::reopen()
    {
        if (openFile.empty() || db == nullptr)
            throw DBException("Cannot reopen unopened database");
        // A new connection, never one kept by close(): reopen() is used to read a schema
        // changed through writable_schema, which an existing connection does not reparse
        closeConnection();
        return this->open(openFile);
    }

    SqliteDatabase &SqliteDatabase::exec(const std::string &sql)
    {
        if (db == nullptr)
            throw DBException("Can't execute SQL: " + sql + ", db is not open");

        char *errMsg = nullptr;
        const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK)
        {
            const std::string error(errMsg ? errMsg : sqlite3_errstr(rc));
            sqlite3_free(errMsg);
            if (rc == SQLITE_INTERRUPT)
                cancel::throwIfCanceled();
            if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED)
                throw DBBusyException(error);
            throw SQLException(error);
        }

        return *this;
    }

    bool SqliteDatabase::tableExists(const std::string &table)
    {
        auto q = query("SELECT count(*) FROM sqlite_master WHERE type='table' AND name=?");
        q->bind(1, table);

        if (q->fetch())
        {
            return q->getInt(0) == 1;
        }

        return false;
    }

    std::string SqliteDatabase::getOpenFile() const
    {
        return openFile;
    }

    // @return  the number of rows modified, inserted or deleted by the
    // most recently completed INSERT, UPDATE or DELETE statement
    int SqliteDatabase::changes()
    {
        return sqlite3_changes(db);
    }

    std::string SqliteDatabase::getJournalMode() const
    {
        auto q = this->query("PRAGMA journal_mode");
        return q->fetch() ? q->getText(0) : "";
    }

    void SqliteDatabase::setJournalMode(const std::string &mode)
    {
        this->exec("PRAGMA journal_mode=" + mode + ";");
    }

    void SqliteDatabase::setWritableSchema(bool enabled)
    {
        this->exec(std::string("PRAGMA writable_schema=") + (enabled ? "on" : "off") + ";");
    }

    bool SqliteDatabase::renameColumnIfExists(const std::string &table, const std::string &columnDefBefore, const std::string &columnDefAfter)
    {
        auto q = this->query("SELECT sql FROM sqlite_master WHERE type = 'table' AND name = ?");
        q->bind(1, table);

        if (q->fetch())
        {
            std::string sqlDef = q->getText(0);

            if (sqlDef.size() > 0 && sqlDef.find(columnDefBefore + ",") != std::string::npos)
            {
                // Old definition
                str::replaceAll(sqlDef, columnDefBefore, columnDefAfter);

                this->setWritableSchema(true);
                q = this->query("UPDATE sqlite_master SET sql = ? WHERE type = 'table' and name = ?");
                q->bind(1, sqlDef);
                q->bind(2, table);
                q->execute();
                this->setWritableSchema(false);
                LOGD << "Updated " << table << " schema definition: " << sqlDef;
                return true;
            }
        }

        return false;
    }

    std::unique_ptr<Statement> SqliteDatabase::query(const std::string &query) const
    {
        return std::make_unique<Statement>(db, query);
    }

    int SqliteDatabase::getUserVersion() const
    {
        auto q = this->query("PRAGMA user_version");
        return q->fetch() ? q->getInt(0) : 0;
    }

    void SqliteDatabase::setUserVersion(int version)
    {
        // PRAGMA doesn't support parameter binding, must use string concatenation
        // Version is an int, so no SQL injection risk
        this->query("PRAGMA user_version = " + std::to_string(version))->execute();
    }

    bool SqliteDatabase::isReadOnly() const
    {
        if (db == nullptr)
            throw DBException("Cannot check read-only state, db is not open");
        return sqlite3_db_readonly(db, "main") == 1;
    }

    SqliteDatabase::~SqliteDatabase()
    {
        this->closeConnection();
    }

}
