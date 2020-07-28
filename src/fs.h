/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

// Compares an extension with a list of extension strings
// @return true if the extension matches one of those in the list
bool checkExtension(const fs::path &extension, const std::initializer_list<std::string>& matches);

time_t getModifiedTime(const std::string &filePath);
off_t getSize(const std::string &filePath);
bool pathsAreChildren(const fs::path &parentPath, const std::vector<std::string> &childPaths);
bool pathIsChild(const fs::path &parentPath, const fs::path &p);
int pathDepth(const fs::path &path);

fs::path getExeFolderPath();
fs::path getDataPath(const fs::path &p);
fs::path getCwd();

// Prints to the provided buffer a nice number of bytes (KB, MB, GB, etc)
std::string bytesToHuman(off_t bytes);

#endif // FILESYSTEM_H
