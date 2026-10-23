/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef MIO_H
#define MIO_H

// "My" I/O library
// io.h was taken :/

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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

            // The path in UTF-8 with '/' separators (toGenericUtf8)
            DDB_DLL std::string generic() const;

            // The path in UTF-8 (toUtf8)
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

        // Owns a temporary path (a directory tree, or a single file) and removes it when the
        // scope exits, on success and on a throw alike, unless release()d (the path was moved
        // into place or handed to another owner). The destructor never throws: a removal
        // failure is logged. Movable, not copyable.
        class DDB_DLL ScopedTempDir
        {
            fs::path path_;

        public:
            ScopedTempDir() = default;
            // Takes ownership of p (it need not exist yet)
            explicit ScopedTempDir(fs::path p) noexcept : path_(std::move(p)) {}
            ~ScopedTempDir();

            ScopedTempDir(const ScopedTempDir &) = delete;
            ScopedTempDir &operator=(const ScopedTempDir &) = delete;
            ScopedTempDir(ScopedTempDir &&other) noexcept;
            ScopedTempDir &operator=(ScopedTempDir &&other) noexcept;

            // Creates the folder <parent>/<prefix><16 random characters> and owns it
            static ScopedTempDir create(const fs::path &parent, const std::string &prefix);

            // Removes the owned path (if any; never throws), then owns p
            void reset(fs::path p = fs::path()) noexcept;
            // Gives up ownership: the path is kept and returned
            fs::path release() noexcept;

            const fs::path &path() const noexcept { return path_; }
            bool empty() const noexcept { return path_.empty(); }
        };

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

        // Writes content to target so that a reader sees the previous file or the new one, never a
        // part: into a sibling "<target><tempTag><rand16>", renamed onto target (replacing it, also
        // on Windows). The sibling is removed and the previous file kept on any failure (a full
        // disk included). `what` names the file in the FSException messages ("Cannot open <what>
        // <path>"). ownerOnly: the file is readable and writable by its owner only (0600), also when
        // target already existed with wider permissions; the sibling has that mode before any
        // content is written to it. POSIX only: a Windows file keeps the access rights of its
        // folder (the user profile folders of the registry credentials are the owner's).
        DDB_DLL void writeFileAtomic(const fs::path &target, const std::string &content, const std::string &what,
                                     bool ownerOnly = false, const std::string &tempTag = ".tmp-");

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

        // The whole content of a file; throws FSException when it cannot be read
        DDB_DLL std::vector<uint8_t> readAllBytes(const fs::path &p);

        // Prints to the provided buffer a nice number of bytes (KB, MB, GB, etc)
        DDB_DLL std::string bytesToHuman(std::uintmax_t bytes);

        // Computes the common directory path (if any) among the given paths
        DDB_DLL fs::path commonDirPath(const std::vector<fs::path> &paths);
        size_t componentsCount(const fs::path &p);

        // ---------------------------------------------------------------- Path encoding
        //
        // Inside libddb a path held in a std::string (or a const char *) is UTF-8, on every
        // platform: the C API receives UTF-8 (XI-03), the index stores UTF-8, GDAL, PROJ,
        // SQLite, PDAL, libzip, Assimp and KTX take UTF-8 file names, and logs and JSON are
        // UTF-8. A string becomes an fs::path only through fromUtf8, and an fs::path becomes a
        // string only through toUtf8 / toGenericUtf8: never fs::path(std::string), an implicit
        // std::string -> fs::path conversion (fs::exists(str), dir / str, std::ifstream(str)),
        // path.string() or path.generic_string(). On Windows (MSVC) those convert with the
        // ANSI code page or the CRT locale, so "C:\data\città" would be mangled, or throw.
        // String literals and ASCII constants ("cog", ".ddb", hashes) are safe either way.
        //
        // Win32 calls use the W variants with the wide form of a path: path.c_str(), or
        // toWide for a string that is not a path. On POSIX a path is bytes: every helper
        // copies them unchanged, without any validation, so Linux and macOS behave exactly
        // as path(std::string) / path.string(). On Windows an invalid UTF-8 sequence (or an
        // unpaired surrogate in a wide string) throws std::system_error.
        //
        // scripts/check-narrow-paths.py keeps the raw conversions from coming back (CI).

        // The path of UTF-8 bytes
        DDB_DLL fs::path fromUtf8(std::string_view utf8);
        // The paths of UTF-8 strings
        DDB_DLL std::vector<fs::path> fromUtf8(const std::vector<std::string> &utf8);
        // The UTF-8 bytes of a path (native separators)
        DDB_DLL std::string toUtf8(const fs::path &p);
        // The UTF-8 bytes of a path with '/' separators (path.generic_string())
        DDB_DLL std::string toGenericUtf8(const fs::path &p);

        // p with UTF-8 text appended to its last component ("a/b.tif" + ".stats.json"):
        // path.concat / += of a UTF-8 string
        DDB_DLL fs::path concat(const fs::path &p, std::string_view utf8);

        // The value of the environment variable `name` in UTF-8, or std::nullopt when it is
        // not set. On Windows getenv returns it in the ANSI code page: this reads the wide
        // environment instead. For values that are paths (or lists of paths, like PATH).
        DDB_DLL std::optional<std::string> getEnvUtf8(const char *name);

#ifdef _WIN32
        // UTF-8 <-> UTF-16, for the Win32 W functions and wide CRT calls
        DDB_DLL std::wstring toWide(std::string_view utf8);
        DDB_DLL std::string fromWide(std::wstring_view wide);
#endif

    }
}
#endif // MIO_H
