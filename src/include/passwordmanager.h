/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef PASSWORDMANAGER_H
#define PASSWORDMANAGER_H

#include <fstream>
#include "fs.h"
#include "json_fwd.h"
#include "database.h"

namespace ddb
{

#define SALT_LENGTH 8

    class PasswordManager
    {

        Database *db;

        int countPasswords();

    public:
        PasswordManager(ddb::Database *db) : db(db) {}

        void append(const std::string &password);

        // Nice to have
        // void remove(const std::string& password);
        bool verify(const std::string &password);
        void clearAll();
    };

}

#endif // PASSWORDMANAGER_H
