/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef AUTHCREDENTIALS_H
#define AUTHCREDENTIALS_H

#include <string>
#include "ddb_export.h"

struct AuthCredentials{
    std::string username;
    std::string password;

    DDB_DLL AuthCredentials();
    DDB_DLL AuthCredentials(const std::string &username, const std::string &password);
    DDB_DLL bool empty() const;

};

#endif // AUTHCREDENTIALS_H
