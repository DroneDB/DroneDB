/* Copyright 2019 MasseranoLabs LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include "userprofile.h"
#include "exceptions.h"

using namespace ddb;

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
}

void UserProfile::createDir(const fs::path &dir){
    if (!fs::exists(dir)){
        if (fs::create_directory(dir)){
            LOGD << "Created " << dir.string();
        }else{
            throw AppException("Cannot create profile directory: " + dir.string() + ". Check that you have permissions to write.");
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

fs::path UserProfile::getProfileDir(){
    return getHomeDir() / ".ddb";
}

fs::path UserProfile::getProfilePath(const fs::path &p, bool createIfNeeded = true){
    const fs::path profilePath = getProfileDir() / p;
    if (createIfNeeded) createDir(profilePath);
    return profilePath;
}

fs::path UserProfile::getThumbsDir(int thumbSize){
    const fs::path thumbsDir = getProfileDir() / fs::path("thumbs");
    createDir(thumbsDir);
    const fs::path thumbsSizeDir = thumbsDir / fs::path(std::to_string(thumbSize));
    createDir(thumbsSizeDir);
    return thumbsSizeDir;
}
