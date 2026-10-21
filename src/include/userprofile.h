/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef USERPROFILE_H
#define USERPROFILE_H

#include <mutex>

#include "fs.h"
#include "logger.h"
#include "authmanager.h"
#include "ddb_export.h"

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
        DDB_DLL static UserProfile *get();

        // The profile folder from the environment, without creating it: DDB_HOME/.ddb when
        // DDB_HOME is set (*fromDdbHome = true), else the home folder's .ddb. Throws
        // AppException when no home folder is set.
        DDB_DLL static fs::path defaultProfileDir(bool *fromDdbHome = nullptr);

        DDB_DLL fs::path getHomeDir();
        DDB_DLL fs::path getProfileDir();
        DDB_DLL fs::path getProfilePath(const fs::path &p, bool createIfNeeded);
        // The cache folder (created) and a folder in it (created)
        DDB_DLL fs::path getCacheDir();
        DDB_DLL fs::path getCachePath(const fs::path &p);
        DDB_DLL fs::path getThumbsDir();
        DDB_DLL fs::path getThumbsDir(int thumbSize);
        DDB_DLL fs::path getTilesDir();
        DDB_DLL fs::path getTemplatesDir();

        DDB_DLL fs::path getAuthFile();

        // Created on first use, with the profile folder and auth.json
        DDB_DLL AuthManager *getAuthManager();

    private:
        UserProfile() = default;

        void createDir(const fs::path &p);

        std::once_flag authOnce;
        AuthManager *authManager = nullptr;
    };

}

#endif // USERPROFILE_H
