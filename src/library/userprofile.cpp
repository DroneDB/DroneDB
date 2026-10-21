/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "userprofile.h"

//#include "ddb.h"

#include "exceptions.h"
#include "process_config.h"

namespace ddb
{

    UserProfile *UserProfile::get()
    {
        // Thread-safe initialization. Never destroyed (as before): code running during
        // static destruction may still ask for the profile.
        static UserProfile *const instance = new UserProfile();
        return instance;
    }

    void UserProfile::createDir(const fs::path &dir)
    {
        if (!fs::exists(dir))
        {
            if (fs::create_directories(dir))
            {
                LOGD << "Created " << dir.string();
            }
            else
            {
                // Was it created by a competing process?
                if (!fs::exists(dir))
                    throw AppException("Cannot create profile directory: " + dir.string() + ". Check that you have permissions to write.");
                LOGD << "Dir was already created (by another process?): " << dir.string();
            }
        }
        else
        {
            LOGD << dir.string() << " exists";
        }
    }

    namespace
    {
        // DDB_HOME (*fromDdbHome = true), else the user's home folder
        fs::path homeDir(bool *fromDdbHome)
        {
            if (fromDdbHome != nullptr)
                *fromDdbHome = false;
            const char *ddb_home = std::getenv("DDB_HOME");
            if (ddb_home)
            {
                if (fromDdbHome != nullptr)
                    *fromDdbHome = true;
                return std::string(ddb_home);
            }

            const char *home = std::getenv("HOME");
            if (home)
                return std::string(home);

            home = std::getenv("USERPROFILE");
            if (home)
                return std::string(home);

            home = std::getenv("HOMEDRIVE");
            const char *homePath = std::getenv("HOMEPATH");

            if (!home || !homePath)
            {
                throw AppException("Cannot find home directory. Make sure that either your HOME or USERPROFILE environment variable is set and points to the current user's home directory.");
            }

            return fs::path(std::string(home)) / fs::path(std::string(homePath));
        }
    } // namespace

    fs::path UserProfile::defaultProfileDir(bool *fromDdbHome) { return homeDir(fromDdbHome) / DDB_FOLDER; }

    fs::path UserProfile::getHomeDir() { return homeDir(nullptr); }

    fs::path UserProfile::getProfileDir() { return defaultProfileDir(); }

    fs::path UserProfile::getProfilePath(const fs::path &p, bool createIfNeeded = true)
    {
        const fs::path profilePath = getProfileDir() / p;
        if (createIfNeeded)
            createDir(profilePath);
        return profilePath;
    }

    fs::path UserProfile::getCacheDir()
    {
        const fs::path cacheDir = ProcessConfig::get().cacheDir();
        createDir(cacheDir);
        return cacheDir;
    }

    fs::path UserProfile::getCachePath(const fs::path &p)
    {
        const fs::path cachePath = getCacheDir() / p;
        createDir(cachePath);
        return cachePath;
    }

    fs::path UserProfile::getThumbsDir()
    {
        const fs::path thumbsDir = getCacheDir() / fs::path("thumbs");
        createDir(thumbsDir);
        return thumbsDir;
    }

    fs::path UserProfile::getThumbsDir(int thumbSize)
    {
        const fs::path thumbsSizeDir = getThumbsDir() / fs::path(std::to_string(thumbSize));
        createDir(thumbsSizeDir);
        return thumbsSizeDir;
    }

    fs::path UserProfile::getTilesDir()
    {
        const fs::path tilesDir = getCacheDir() / fs::path("tiles");
        createDir(tilesDir);
        return tilesDir;
    }

    fs::path UserProfile::getTemplatesDir()
    {
        const fs::path templatesDir = getCacheDir() / fs::path("templates");
        createDir(templatesDir);
        return templatesDir;
    }

    fs::path UserProfile::getAuthFile()
    {
        return getProfileDir() / "auth.json";
    }

    AuthManager *UserProfile::getAuthManager()
    {
        std::call_once(authOnce, [this]() {
            createDir(getProfileDir());
            authManager = new AuthManager(getAuthFile());
        });
        return authManager;
    }

}
