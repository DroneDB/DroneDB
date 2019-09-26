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
#ifndef DATABASE_H
#define DATABASE_H
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

class Database{
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

    std::unique_ptr<Statement> query(const std::string &query);

    ~Database();
};

#endif // DATABASE_H
