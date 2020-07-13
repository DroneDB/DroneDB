/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef AUTHMANAGER_H
#define AUTHMANAGER_H

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace ddb{

class AuthManager{
    fs::path authFile;


    void ReadFromDisk();
    void WriteToDisk();
public:
    AuthManager(const fs::path &authFile);
};

}

#endif // AUTHMANAGER_H
