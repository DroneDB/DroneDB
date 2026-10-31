/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef USERPROFILE_H
#define USERPROFILE_H

#include <mutex>

#include "fs.h"
#include "logger.h"
#include "authmanager.h"

#define DDB_FOLDER ".ddb"

namespace ddb
{

    // The user's profile folder (DDB_HOME/.ddb, else the home folder's .ddb), which holds the
    // registry credentials (auth.json), and libddb's cache folder (ProcessConfig::cacheDir:
    // DDBConfigure "cacheDir", else the profile folder), which holds the thumbnails, tiles,
    // index templates and DSM tiles. Folders are created when first asked for.
    class UserProfile
    {
    public:
        static UserProfile *get();

        // The profile folder from the environment, without creating it: DDB_HOME/.ddb when
        // DDB_HOME is set (*fromDdbHome = true), else the home folder's .ddb. Throws
        // AppException when no home folder is set.
        static fs::path defaultProfileDir(bool *fromDdbHome = nullptr);

        fs::path getProfileDir();
        // The cache folder (created) and a folder in it (created)
        fs::path getCacheDir();
        fs::path getCachePath(const fs::path &p);
        fs::path getThumbsDir();
        fs::path getTilesDir();
        fs::path getTemplatesDir();

        fs::path getAuthFile();

        // Created on first use, with the profile folder and auth.json
        AuthManager *getAuthManager();

    private:
        UserProfile() = default;

        void createDir(const fs::path &p);

        std::once_flag authOnce;
        AuthManager *authManager = nullptr;
    };

}

#endif // USERPROFILE_H
