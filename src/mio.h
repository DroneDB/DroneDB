/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef MIO_H
#define MIO_H

// "My" I/O library
// io.h was taken :/

#include <algorithm>
#include "fs.h"

#ifdef WIN32
#define stat _stat
#endif

namespace fs = std::filesystem;

namespace ddb{
namespace io{

class Path{
    fs::path p;

public:
    Path(const fs::path &p) : p(p) {}

    // Compares an extension with a list of extension strings
    // @return true if the extension matches one of those in the list
    bool checkExtension(const std::initializer_list<std::string>& matches);

    time_t getModifiedTime();
    std::uintmax_t getSize();
    bool hasChildren(const std::vector<std::string> &childPaths);
    bool isParentOf(const fs::path &childPath);
    int depth();

    // Computes a relative path to parent
    // Taking care of edge cases between platforms
    // and canonicalizing the path
    Path relativeTo(const fs::path &parent);

    // Cross-platform path.generic_string()
    std::string generic() const;

    std::string string() const;
    fs::path get() const{ return p; }
};

fs::path getExeFolderPath();
fs::path getDataPath(const fs::path &p);
fs::path getCwd();

// Prints to the provided buffer a nice number of bytes (KB, MB, GB, etc)
std::string bytesToHuman(std::uintmax_t bytes);

}
}
#endif // MIO_H
