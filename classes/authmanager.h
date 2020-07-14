/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef AUTHMANAGER_H
#define AUTHMANAGER_H

#include <fstream>
#include "authcredentials.h"
#include "../fs.h"
#include "../json.h"

namespace ddb{

class AuthManager{
    json auth;
    fs::path authFile;

    void ReadFromDisk();
    void WriteToDisk();
public:
    AuthManager(const fs::path &authFile);

    void saveCredentials(const std::string &url, const AuthCredentials &creds);
    AuthCredentials loadCredentials(const std::string &url);
    bool deleteCredentials(const std::string &url);

    std::vector<std::string> getAuthenticatedRegistryUrls();
};

}

#endif // AUTHMANAGER_H
