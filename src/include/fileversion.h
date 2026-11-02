/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef FILEVERSION_H
#define FILEVERSION_H

#include <string>

#include "fs.h"

namespace ddb
{
    /// Version of the file at path: changes when the file is rewritten, replaced or
    /// deleted (modification time in nanoseconds, size and, on POSIX, device and inode).
    /// Empty when the file does not exist. Caches of data derived from a file key their
    /// entries with it, so a rebuilt file is never served from a stale entry.
    std::string fileVersion(const fs::path &file);

    /// Identity of a file that changes when the path points to another file: device and
    /// inode on POSIX; on Windows, where an open file cannot be replaced, a constant.
    /// Empty when the file does not exist.
    std::string fileIdentity(const fs::path &file);

    /// Absolute, normalized form of path with '/' separators: the key that path-scoped
    /// caches use, so releasing a folder matches every entry under it.
    std::string cacheKeyPath(const fs::path &path);

    /// True when key (a cacheKeyPath) is prefix itself or lies under the folder prefix
    bool isCacheKeyUnder(const std::string &key, const std::string &prefix);
}

#endif // FILEVERSION_H
