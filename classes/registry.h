/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef REGISTRY_H
#define REGISTRY_H

#include <string>

namespace ddb{

class Registry{
    std::string url;

    // Last valid token
    std::string token;
public:
    Registry(const std::string &url);

    std::string getUrl() const;

    std::string getToken(const std::string &username, const std::string &password) const;
    std::string login(const std::string &username, const std::string &password) const;
    void logout();
};

}

#endif // REGISTRY_H
