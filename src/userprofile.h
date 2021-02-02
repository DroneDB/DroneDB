/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef USERPROFILE_H
#define USERPROFILE_H

#include "fs.h"
#include "logger.h"
#include "authmanager.h"
#include "ddb_export.h"

namespace ddb{

class UserProfile{
public:
    DDB_DLL static UserProfile* get();

    DDB_DLL fs::path getHomeDir();
    DDB_DLL fs::path getProfileDir();
    DDB_DLL fs::path getProfilePath(const fs::path &p, bool createIfNeeded);
    DDB_DLL fs::path getThumbsDir();
    DDB_DLL fs::path getThumbsDir(int thumbSize);
    DDB_DLL fs::path getTilesDir();
    DDB_DLL fs::path getTemplatesDir();

    DDB_DLL fs::path getAuthFile();

    DDB_DLL AuthManager *getAuthManager();
private:
    UserProfile();

    void createDir(const fs::path &p);

    static UserProfile *instance;

    AuthManager *authManager;
};

}

#endif // USERPROFILE_H
