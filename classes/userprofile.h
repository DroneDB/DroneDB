/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef USERPROFILE_H
#define USERPROFILE_H

#include <filesystem>
#include "../logger.h"

namespace fs = std::filesystem;

class UserProfile{
public:
    static UserProfile* get();

    fs::path getHomeDir();
    fs::path getProfileDir();
    fs::path getProfilePath(const fs::path &p, bool createIfNeeded);
    fs::path getThumbsDir(int thumbSize);
private:
    UserProfile();

    void createDir(const fs::path &p);

    static UserProfile *instance;
};

#endif // USERPROFILE_H
