/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DATABASE_H
#define DATABASE_H
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Spatialite includes
#include <geos_c.h>
#include <sqlite3.h>
#include <spatialite/gaiageo.h>
#include <spatialite.h>
#include <filesystem>

#include <string>
#include <memory>

#include "statement.h"

namespace fs = std::filesystem;

class Database {
    sqlite3 *db;
    std::string openFile;
  public:
    static void Initialize();

    Database();
    Database &open(const std::string &file);
    Database &close();
    Database &exec(const std::string &sql);
    Database &createTables();
    bool tableExists(const std::string &table);
    std::string getOpenFile();
    int changes();

    std::unique_ptr<Statement> query(const std::string &query);

    ~Database();
};

#endif // DATABASE_H
