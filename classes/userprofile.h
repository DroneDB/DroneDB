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
private:
    UserProfile();

    void createDir(const fs::path &p);

    static UserProfile *instance;
};

#endif // USERPROFILE_H
