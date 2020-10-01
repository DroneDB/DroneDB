/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef MIO_H
#define MIO_H

// "My" I/O library
// io.h was taken :/

#include <algorithm>
#include "fs.h"
#include "ddb_export.h"

#ifdef WIN32
#define stat _stat
#endif

namespace fs = std::filesystem;

namespace ddb{
namespace io{

class Path{
    fs::path p;

public:
    DDB_DLL Path(const fs::path &p) : p(p) {}

    // Compares an extension with a list of extension strings
    // @return true if the extension matches one of those in the list
    DDB_DLL bool checkExtension(const std::initializer_list<std::string>& matches);

    DDB_DLL time_t getModifiedTime();
    DDB_DLL std::uintmax_t getSize();
    DDB_DLL bool hasChildren(const std::vector<std::string> &childPaths);
    DDB_DLL bool isParentOf(const fs::path &childPath);
    DDB_DLL bool isAbsolute() const;
    DDB_DLL bool isRelative() const;
    DDB_DLL int depth();

    // Computes a relative path to parent
    // Taking care of edge cases between platforms
    // and canonicalizing the path
    DDB_DLL Path relativeTo(const fs::path &parent);

    // Remove the root path to make this
    // path relative to its root
    // (path must be absolute)
    DDB_DLL Path withoutRoot();

    // Cross-platform path.generic_string()
    DDB_DLL std::string generic() const;

    DDB_DLL std::string string() const;
    DDB_DLL fs::path get() const{ return p; }
};

DDB_DLL fs::path getExeFolderPath();
DDB_DLL fs::path getDataPath(const fs::path &p);
DDB_DLL fs::path getCwd();

// Prints to the provided buffer a nice number of bytes (KB, MB, GB, etc)
DDB_DLL std::string bytesToHuman(std::uintmax_t bytes);

}
}
#endif // MIO_H
