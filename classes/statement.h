/* Copyright 2019 MasseranoLabs LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#ifndef STATEMENT_H
#define STATEMENT_H

#include <sqlite3.h>
#include <string>
#include "../logger.h"

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

    void reset();
};

#endif // STATEMENT_H
