/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef MIO_H
#define MIO_H

// "My" I/O library
// io.h was taken :/

#include <algorithm>
#include <limits>
#include <fcntl.h>
#include "fs.h"
#include "ddb_export.h"
#include "hash.h"

#ifdef WIN32
#define stat _stat
#endif

#ifdef WIN32
#include <io.h> // _get_osfhandle
#define LOCK_EX 2
#define LOCK_NB 4
#else
#include <sys/file.h>
#define _close close
#define _unlink unlink
#define _open open
#endif

namespace fs = std::filesystem;

namespace ddb{
namespace io{

class Path{
    fs::path p;

public:
    DDB_DLL Path(){}
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

class FileLock{
    int fd;
    std::string lockFile;
public:
    DDB_DLL FileLock(const fs::path &p);
    DDB_DLL ~FileLock();
};

#ifdef WIN32
// emulate flock
int flock (int fd, int operation);
#endif

DDB_DLL fs::path getExeFolderPath();
DDB_DLL fs::path getDataPath(const fs::path &p);
DDB_DLL fs::path getCwd();
DDB_DLL fs::path assureFolderExists(const fs::path &d);
DDB_DLL void createDirectories(const fs::path &d);
DDB_DLL void assureIsRemoved(const fs::path p);

// Prints to the provided buffer a nice number of bytes (KB, MB, GB, etc)
DDB_DLL std::string bytesToHuman(std::uintmax_t bytes);

// Computes the common directory path (if any) among the given paths
DDB_DLL fs::path commonDirPath(const std::vector<fs::path> &paths);
size_t componentsCount(const fs::path &p);

}
}
#endif // MIO_H
