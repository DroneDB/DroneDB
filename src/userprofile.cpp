/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "userprofile.h"

#include <ddb.h>

#include "exceptions.h"

namespace ddb{

UserProfile *UserProfile::instance = nullptr;

UserProfile *UserProfile::get(){
    if (!instance){
        instance = new UserProfile();
    }

    return instance;
}

UserProfile::UserProfile(){
    // Initialize directories
    createDir(getProfileDir());

    // Initialize auth manager
    authManager = new AuthManager(getAuthFile());
}

void UserProfile::createDir(const fs::path &dir){
    if (!fs::exists(dir)){
        if (fs::create_directory(dir)){
            LOGD << "Created " << dir.string();
        }else{
            // Was it created by a competing process?
            if (!fs::exists(dir)) throw AppException("Cannot create profile directory: " + dir.string() + ". Check that you have permissions to write.");
            LOGD << "Dir was already created (by another process?): " << dir.string();
        }
    }else{
        LOGD << dir.string() << " exists";
    }
}

fs::path UserProfile::getHomeDir()
{
    const char *home = std::getenv("HOME");
    if (home) return std::string(home);

    home = std::getenv("USERPROFILE");
    if (home) return std::string(home);

    home = std::getenv("HOMEDRIVE");
    const char *homePath = std::getenv("HOMEPATH");

    if (!home || !homePath){
        throw AppException("Cannot find home directory. Make sure that either your HOME or USERPROFILE environment variable is set and points to the current user's home directory.");
    }

    return fs::path(std::string(home)) / fs::path(std::string(homePath));
}

fs::path UserProfile::getProfileDir(){ return getHomeDir() / DDB_FOLDER; }

fs::path UserProfile::getProfilePath(const fs::path &p, bool createIfNeeded = true){
    const fs::path profilePath = getProfileDir() / p;
    if (createIfNeeded) createDir(profilePath);
    return profilePath;
}

fs::path UserProfile::getThumbsDir(){
    const fs::path thumbsDir = getProfileDir() / fs::path("thumbs");
    createDir(thumbsDir);
    return thumbsDir;
}

fs::path UserProfile::getThumbsDir(int thumbSize){
    const fs::path thumbsSizeDir = getThumbsDir() / fs::path(std::to_string(thumbSize));
    createDir(thumbsSizeDir);
    return thumbsSizeDir;
}

fs::path UserProfile::getTilesDir(){
    const fs::path tilesDir = getProfileDir() / fs::path("tiles");
    createDir(tilesDir);
    return tilesDir;
}

fs::path UserProfile::getTemplatesDir(){
    const fs::path tilesDir = getProfileDir() / fs::path("templates");
    createDir(tilesDir);
    return tilesDir;
}

fs::path UserProfile::getAuthFile(){
    return getProfileDir() / "auth.json";
}

AuthManager *UserProfile::getAuthManager(){
    return authManager;
}

}
