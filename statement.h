#ifndef STATEMENT_H
#define STATEMENT_H

#include <sqlite3.h>
#include <string>
#include "logger.h"

class Statement{
    sqlite3 *db;
    std::string query;

    bool hasRow;
    bool done;

    sqlite3_stmt *stmt;

    void bindCheck(int ret);
    Statement &step();
public:
    Statement(sqlite3 *db, const std::string &query);
    ~Statement();

    Statement &bind(int paramNum, const std::string &value);
    // TODO: more

    bool fetch();

    int getInt(int columnId);
    std::string getText(int columnId);
    // TODO: more

    // TODO: reset via sqlite3_reset
};

#endif // STATEMENT_H
