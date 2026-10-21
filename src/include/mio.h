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
#include <vector>
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
#include <utime.h>
#define _close close
#define _unlink unlink
#define _open open
#endif

namespace fs = std::filesystem;

namespace ddb
{
    namespace io
    {

        // What one stat() of a path returns
        struct FileState
        {
            bool directory = false;
            std::uintmax_t size = 0;
            double mtime = 0; // seconds, with the sub-second part the file system keeps
        };

        class Path
        {
            fs::path p;

        public:
            DDB_DLL Path() {}
            DDB_DLL Path(const fs::path &p) : p(p) {}

            // Compares an extension with a list of extension strings
            // @return true if the extension matches one of those in the list
            DDB_DLL bool checkExtension(const std::initializer_list<std::string> &matches) const;

            DDB_DLL time_t getModifiedTime();
            DDB_DLL bool setModifiedTime(time_t mtime);

            // Type, size and modification time in one system call; throws FSException
            DDB_DLL FileState getState();
            // Modification time in seconds, with the sub-second part the file system keeps
            DDB_DLL double getModifiedTimePrecise();
            // Sets the modification time unless sameModifiedTime() already holds; returns true if set
            DDB_DLL bool setModifiedTimePrecise(double mtime);
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
            DDB_DLL fs::path get() const { return p; }
        };

        class FileLock
        {
            int fd = -1;
            std::string lockFile;

        public:
            DDB_DLL FileLock();
            DDB_DLL FileLock(const fs::path &p);
            DDB_DLL ~FileLock();

            DDB_DLL void lock(const fs::path &p);
            DDB_DLL void unlock();
        };

#ifdef WIN32
        // emulate flock
        int flock(int fd, int operation);
#endif

        // Whether a modification time stored in an index matches the file's (getModifiedTimePrecise).
        // A whole number is compared to the second (rows written before sub-second times were
        // stored, and file systems that keep seconds only); otherwise to the microsecond.
        DDB_DLL bool sameModifiedTime(double stored, double file);

        DDB_DLL fs::path getExeFolderPath();
        DDB_DLL fs::path getDataPath(const fs::path &p);
        DDB_DLL fs::path getCwd();
        DDB_DLL fs::path assureFolderExists(const fs::path &d);
        DDB_DLL void createDirectories(const fs::path &d);
        DDB_DLL void assureIsRemoved(const fs::path p);
        DDB_DLL void copy(const fs::path &from, const fs::path &to);
        DDB_DLL void hardlink(const fs::path &target, const fs::path &linkName);
        DDB_DLL void hardlinkSafe(const fs::path &target, const fs::path &linkName);
        DDB_DLL void rename(const fs::path &from, const fs::path &to);

        // Moves the file `from` to `to`, replacing `to`. Renames it when it is the only link to
        // its data; copies it and removes `from` when it has other hard links (so `to` never
        // shares its data with another path) or when the rename fails (another volume).
        // Returns true when the file was renamed.
        DDB_DLL bool moveOrCopy(const fs::path &from, const fs::path &to);
        DDB_DLL void remove(const fs::path p);
        DDB_DLL bool exists(const fs::path p);

        // Whether a and b name the same file: fs::equivalent when both exist (symlinks, hard
        // links, case-insensitive names), otherwise their weakly canonical absolute forms
        // (relative paths, "." and ".." segments, symlinked parent folders).
        DDB_DLL bool isSamePath(const fs::path &a, const fs::path &b);

        // Throws InvalidArgsException when output names the same file as input. A writer that
        // replaces an existing output would otherwise delete or truncate its input before
        // reading it.
        DDB_DLL void assureDistinctOutput(const fs::path &input, const fs::path &output);

        // Prints to the provided buffer a nice number of bytes (KB, MB, GB, etc)
        DDB_DLL std::string bytesToHuman(std::uintmax_t bytes);

        // Computes the common directory path (if any) among the given paths
        DDB_DLL fs::path commonDirPath(const std::vector<fs::path> &paths);
        size_t componentsCount(const fs::path &p);

    }
}
#endif // MIO_H
