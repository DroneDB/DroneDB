/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef REGISTRY_H
#define REGISTRY_H

#include <string>
#include <vector>
#include "constants.h"
#include "ddb_export.h"
#include "net.h"
#include <chrono>

namespace ddb{

class Registry{
    std::string url;
    
    // Last valid token
    std::string authToken;
    
    time_t tokenExpiration;

   public:
    DDB_DLL Registry(const std::string &url = DEFAULT_REGISTRY);

    DDB_DLL std::string getUrl(const std::string &path = "") const;
    DDB_DLL std::string login();

    DDB_DLL std::string getAuthToken();
    DDB_DLL std::string login(const std::string &username, const std::string &password);
    DDB_DLL void ensureTokenValidity();
    DDB_DLL bool logout();
    DDB_DLL void clone(const std::string &organization, const std::string &dataset,
                       const std::string &folder);

    DDB_DLL void handleError(net::Response &res);

    DDB_DLL time_t getTokenExpiration();

};

}

#endif // REGISTRY_H
