/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef PASSWORDMANAGER_H
#define PASSWORDMANAGER_H

#include <fstream>
#include "fs.h"
#include "json.h"
#include "database.h"

namespace ddb {

#define SALT_LENGTH 8

class PasswordManager {

    Database* db;

    DDB_DLL int countPasswords();

public:
    PasswordManager(ddb::Database* db) {
        this->db = db;

        if (!db->tableExists("passwords"))
            db->createTables();
    }

    DDB_DLL void append(const std::string& password);

    // Nice to have
    //void remove(const std::string& password);
    DDB_DLL bool verify(const std::string& password);
    DDB_DLL void clearAll();

};

}

#endif // PASSWORDMANAGER_H
