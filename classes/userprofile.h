/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef USERPROFILE_H
#define USERPROFILE_H

#include "../fs.h"
#include "../logger.h"
#include "authmanager.h"

namespace ddb{

class UserProfile{
public:
    static UserProfile* get();

    fs::path getHomeDir();
    fs::path getProfileDir();
    fs::path getProfilePath(const fs::path &p, bool createIfNeeded);
    fs::path getThumbsDir(int thumbSize);
    fs::path getAuthFile();

    AuthManager *getAuthManager();
private:
    UserProfile();

    void createDir(const fs::path &p);

    static UserProfile *instance;

    AuthManager *authManager;
};

}

#endif // USERPROFILE_H
