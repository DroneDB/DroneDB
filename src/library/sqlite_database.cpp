/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <cassert>
#include <string>

#include "logger.h"
#include "exceptions.h"
#include "sqlite_database.h"
#include "utils.h"

namespace ddb
{

    SqliteDatabase::SqliteDatabase() : db(nullptr) {}

    SqliteDatabase &SqliteDatabase::open(const std::string &file)
    {
        if (db != nullptr)
            throw DBException("Can't open database " + file + ", one is already open (" + openFile + ")");
        LOGD << "Opening connection to " << file;
        if (sqlite3_open(file.c_str(), &db) != SQLITE_OK)
            throw DBException("Can't open database: " + file);

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
        if (db != nullptr)
        {
            LOGD << "Closing connection to " << openFile;
            sqlite3_close(db);
            db = nullptr;
        }

        return *this;
    }

    SqliteDatabase &SqliteDatabase::reopen()
    {
        if (openFile.empty() || db == nullptr)
            throw DBException("Cannot reopen unopened database");
        return this->close().open(openFile);
    }

    SqliteDatabase &SqliteDatabase::exec(const std::string &sql)
    {
        if (db == nullptr)
            throw DBException("Can't execute SQL: " + sql + ", db is not open");

        char *errMsg;
        if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg) != SQLITE_OK)
        {
            const std::string error(errMsg);
            sqlite3_free(errMsg);
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
                utils::stringReplace(sqlDef, columnDefBefore, columnDefAfter);

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

    SqliteDatabase::~SqliteDatabase()
    {
        this->close();
    }

}
