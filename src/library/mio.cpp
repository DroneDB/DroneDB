/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "mio.h"
#include "utils.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace ddb
{
    namespace io
    {

        bool Path::checkExtension(const std::initializer_list<std::string> &matches)
        {
            std::string ext = p.extension().string();
            if (ext.size() < 1)
                return false;
            std::string extLowerCase = ext.substr(1, ext.length());
            utils::toLower(extLowerCase);

            for (auto &m : matches)
            {
                std::string ext = m;
                utils::toLower(ext);
                if (ext == extLowerCase)
                    return true;
            }
            return false;
        }

        time_t Path::getModifiedTime()
        {
#ifdef WIN32
            HANDLE hFile;
            FILETIME ftModified;
            hFile = CreateFileW(p.wstring().c_str(), 0, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, NULL);
            if (hFile == INVALID_HANDLE_VALUE)
            {
                throw FSException("Cannot stat mtime (open) " + p.string() + " (errcode: " + std::to_string(GetLastError()) + ")");
            }

            if (!GetFileTime(hFile, NULL, NULL, &ftModified))
            {
                CloseHandle(hFile);
                throw FSException("Cannot stat mtime (get time) " + p.string());
            }

            CloseHandle(hFile);

            // Get the number of seconds since January 1, 1970 12:00am UTC
            LARGE_INTEGER li;
            li.LowPart = ftModified.dwLowDateTime;
            li.HighPart = ftModified.dwHighDateTime;

            const int64_t UNIX_TIME_START = 0x019DB1DED53E8000; // January 1, 1970 (start of Unix epoch) in "ticks"
            const int64_t TICKS_PER_SECOND = 10000000;          // a tick is 100ns

            // Convert ticks since 1/1/1970 into seconds
            return (li.QuadPart - UNIX_TIME_START) / TICKS_PER_SECOND;
#else
            struct stat result;
            if (stat(p.string().c_str(), &result) == 0)
            {
                return result.st_mtime;
            }
            else
            {
                throw FSException("Cannot stat mtime " + p.string());
            }
#endif
        }

        bool Path::setModifiedTime(time_t mtime)
        {
#ifdef WIN32
            if (getModifiedTime() != mtime)
            {
                HANDLE hFile;
                FILETIME ftModified;
                FILETIME ftCreated;
                FILETIME ftAccessed;
                hFile = CreateFileW(p.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_WRITE_ATTRIBUTES, NULL);
                if (hFile == INVALID_HANDLE_VALUE)
                {
                    throw FSException("Cannot stat (open) " + p.string() +
                                      " (errcode: " + std::to_string(GetLastError()) +
                                      ")");
                }

                if (!GetFileTime(hFile, &ftCreated, &ftAccessed, &ftModified))
                {
                    CloseHandle(hFile);
                    throw FSException("Cannot stat mtime (set time) " + p.string());
                }

                const int64_t UNIX_TIME_START =
                    0x019DB1DED53E8000;                    // January 1, 1970 (start of Unix epoch) in
                                                           // "ticks"
                const int64_t TICKS_PER_SECOND = 10000000; // a tick is 100ns

                // Change only mtime
                LARGE_INTEGER li;
                li.QuadPart = (mtime * TICKS_PER_SECOND) + UNIX_TIME_START;

                ftModified.dwLowDateTime = li.LowPart;
                ftModified.dwHighDateTime = li.HighPart;

                if (!SetFileTime(hFile, &ftCreated, &ftAccessed, &ftModified))
                {
                    CloseHandle(hFile);
                    throw FSException("Cannot set mtime " + p.string());
                }

                CloseHandle(hFile);

                return true;
            }
            else
            {
                return false;
            }
#else
            struct stat sres;
            if (stat(p.string().c_str(), &sres) != 0)
            {
                throw FSException("Cannot stat " + p.string());
            }

            if (sres.st_mtime != mtime)
            {
                struct utimbuf t;
                t.modtime = mtime;
                t.actime = sres.st_atime;
                if (utime(p.string().c_str(), &t) != 0)
                {
                    throw FSException("Cannot set mtime " + p.string());
                }
                return true;
            }
            else
            {
                return false;
            }
#endif
        }

        std::uintmax_t Path::getSize()
        {
#ifdef WIN32
            HANDLE hFile;
            hFile = CreateFileW(p.wstring().c_str(), 0, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, NULL);
            if (hFile == INVALID_HANDLE_VALUE)
            {
                throw FSException("Cannot stat size (open) " + p.string() + " (errcode: " + std::to_string(GetLastError()));
            }

            LARGE_INTEGER size;
            if (!GetFileSizeEx(hFile, &size))
            {
                CloseHandle(hFile);
                throw FSException("Cannot stat size (getfilesize) " + p.string());
            }
            CloseHandle(hFile);
            return size.QuadPart;
#else
            struct stat result;
            if (stat(p.string().c_str(), &result) == 0)
            {
                return result.st_size;
            }
            else
            {
                throw FSException("Cannot stat size " + p.string());
            }
#endif
        }

        bool Path::hasChildren(const std::vector<std::string> &childPaths)
        {
            try
            {
                std::string absP = fs::weakly_canonical(fs::absolute(p)).string();
                if (absP.length() > 1 && absP.back() == fs::path::preferred_separator)
                    absP.pop_back();

                for (auto &cp : childPaths)
                {
                    std::string absC = fs::weakly_canonical(fs::absolute(cp)).string();
                    if (absC.length() > 1 && absC.back() == fs::path::preferred_separator)
                        absC.pop_back();
                    if (absC.rfind(absP, 0) != 0 || absP == absC)
                        return false;
                }

                return true;
            }
            catch (const fs::filesystem_error &e)
            {
                LOGD << e.what();
                throw FSException(e.what());
            }
        }

        bool Path::isParentOf(const fs::path &childPath)
        {
            try
            {
                std::string absP = fs::weakly_canonical(fs::absolute(p)).string();
                if (absP.length() > 1 && absP.back() == fs::path::preferred_separator)
                    absP.pop_back();
                std::string absC = fs::weakly_canonical(fs::absolute(childPath)).string();
                if (absC.length() > 1 && absC.back() == fs::path::preferred_separator)
                    absC.pop_back();
                return absC.rfind(absP, 0) == 0 && absP != absC;
            }
            catch (const fs::filesystem_error &e)
            {
                LOGD << e.what();
                throw FSException(e.what());
            }
        }

        bool Path::isAbsolute() const
        {
            return p.is_absolute();
        }

        bool Path::isRelative() const
        {
            return p.is_relative();
        }

        // Counts the number of path components
        // it does NOT normalize the path to account for ".." and "." folders
        int Path::depth()
        {
            int count = 0;
            for (auto const &it : p)
            {
                if (it.c_str()[0] != fs::path::preferred_separator &&
                    it.string() != fs::current_path().root_name())
                    count++;
            }
            return std::max(count - 1, 0);
        }

        Path Path::relativeTo(const fs::path &parent)
        {
            try
            {
                // Special case where parent == path
                if (weakly_canonical(absolute(p)) == weakly_canonical(absolute(parent)))
                {
                    return fs::path("");
                }

                // Handle special cases where root is "/"
                // in this case we return the relative canonical absolute path
                if (parent == parent.root_path() || parent == "/")
                {
                    return fs::weakly_canonical(fs::absolute(p)).relative_path();
                }

                fs::path relPath = relative(weakly_canonical(absolute(p)), weakly_canonical(absolute(parent)));
                if (relPath.generic_string() == ".")
                {
                    return weakly_canonical(absolute(parent));
                }

                return relPath;
            }
            catch (const fs::filesystem_error &e)
            {
                LOGD << e.what();
                throw FSException(e.what());
            }
        }

        Path Path::withoutRoot()
        {
            if (!isAbsolute())
                return Path(p);
            return Path(p.relative_path());
        }

        std::string Path::generic() const
        {
            std::string res = p.generic_string();

            // Remove trailing slash from directory (unless "/")
            if (res.length() > 1 && res[res.length() - 1] == '/')
                res.pop_back();

            return res;
        }

        std::string Path::string() const
        {
            return p.string();
        }

        fs::path getExeFolderPath()
        {
#ifdef WIN32
            wchar_t path[MAX_PATH] = {0};
            GetModuleFileNameW(NULL, path, MAX_PATH);
            return fs::path(path).parent_path();
#elif __APPLE__
            char result[PATH_MAX];
            uint32_t size = sizeof(result);
            if (_NSGetExecutablePath(result, &size) == 0)
            {
                return fs::path(result).parent_path();
            }
            return fs::path("");
#else
            char result[PATH_MAX];
            ssize_t count = readlink("/proc/self/exe", result, PATH_MAX);
            return fs::path(std::string(result, static_cast<size_t>((count > 0) ? count : 0))).parent_path();
#endif
        }

        fs::path getDataPath(const fs::path &p)
        {
            // Attempt to find a path within the places we would
            // usually look for DATA files.
            const char *ddb_data_path = std::getenv("DDB_DATA");
            if (ddb_data_path && fs::exists(fs::path(ddb_data_path) / p))
            {
                return fs::path(ddb_data_path) / p;
            }

            fs::path exePath = getExeFolderPath();
            if (fs::exists(exePath / p))
            {
                return exePath / p;
            }

            fs::path cwd = getCwd();
            if (fs::exists(cwd / "ddb_data" / p))
            {
                return cwd / "ddb_data" / p;
            }

            if (fs::exists(cwd / p))
            {
                return cwd / p;
            }

#ifdef WIN32
            // Check same location as DLL
            char dllPath[MAX_PATH];
            HMODULE hm = NULL;

            if (GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  (LPCSTR)&getDataPath, &hm) == 0)
            {
                int ret = GetLastError();
                LOGD << "GetModuleHandle failed, error = " << ret;
                return "";
            }

            if (GetModuleFileName(hm, dllPath, sizeof(dllPath)) == 0)
            {
                int ret = GetLastError();
                LOGD << "GetModuleFileName failed, error = " << ret;
                return "";
            }

            fs::path dllDir = (fs::path(dllPath)).parent_path();
            if (fs::exists(dllDir / p))
            {
                return dllDir / p;
            }
#else
            // On *nix, check /usr/share /usr/local/share folders
            fs::path usrLocalShare = fs::path("/usr/local/share/ddb");
            if (fs::exists(usrLocalShare / p))
            {
                return usrLocalShare / p;
            }

            fs::path usrShare = fs::path("/usr/share/ddb");
            if (fs::exists(usrShare / p))
            {
                return usrShare / p;
            }

#if __APPLE__
            fs::path hbShare = fs::path("/opt/homebrew/share/ddb");
            if (fs::exists(hbShare / p))
            {
                return hbShare / p;
            }
#endif

#endif

            return "";
        }

        fs::path getCwd()
        {
            char result[PATH_MAX];
#ifdef WIN32
            if (_getcwd(result, PATH_MAX) == NULL)
                throw FSException("Cannot get cwd");
#else
            if (getcwd(result, PATH_MAX) == NULL)
                throw FSException("Cannot get cwd");
#endif
            return fs::path(result);
        }

        std::string bytesToHuman(std::uintmax_t bytes)
        {
            std::ostringstream os;

            const char *suffixes[7];
            suffixes[0] = "B";
            suffixes[1] = "KB";
            suffixes[2] = "MB";
            suffixes[3] = "GB";
            suffixes[4] = "TB";
            suffixes[5] = "PB";
            suffixes[6] = "EB";
            std::uintmax_t s = 0;

            double count = static_cast<double>(bytes);

            while (count >= 1024 && s < 7)
            {
                s++;
                count /= 1024;
            }
            if (count - floor(count) == 0.0)
            {
                os << int(count) << " " << suffixes[s];
            }
            else
            {
                os << std::fixed << std::setprecision(2) << count << " " << suffixes[s];
            }

            return os.str();
        }

        fs::path commonDirPath(const std::vector<fs::path> &paths)
        {
            if (paths.size() == 0)
                return fs::path("");

            // Find smallest path (by number of components)
            fs::path smallest;
            size_t maxComponents = INT_MAX;

            for (auto &p : paths)
            {
                size_t count = componentsCount(p);
                if (count < maxComponents)
                {
                    smallest = p;
                    maxComponents = count;
                }
            }
            size_t matchingComponents = 0;

            while (maxComponents > 0)
            {
                for (size_t i = matchingComponents; i < maxComponents; i++)
                {
                    for (auto it = paths.begin() + 1; it != paths.end(); it++)
                    {
                        if (std::next(it->begin(), i)->string() != std::next(smallest.begin(), i)->string())
                        {
                            smallest = smallest.parent_path();
                            maxComponents--;
                            goto breakout; // Break out of two loops. https://xkcd.com/292/
                        }
                    }
                    matchingComponents = i;
                }

                // We're done
                break;

            breakout:;
            }

            fs::path result;
            for (size_t i = 0; i < maxComponents; i++)
            {
                result = result / *std::next(smallest.begin(), i);
            }

            return result;
        }

        size_t componentsCount(const fs::path &p)
        {
            size_t result = 0;
            for (auto it = p.begin(); it != p.end(); it++)
                result++;
            return result;
        }

        fs::path assureFolderExists(const fs::path &d)
        {
            if (!fs::exists(d))
            {
                // Try to create
                createDirectories(d);
                return d;
            }
            else if (fs::is_directory(d))
            {
                return d;
            }
            else
            {
                throw FSException(d.string() + " is not a valid directory (there might be a file with the same name).");
            }
        }

        void createDirectories(const fs::path &d)
        {
            std::error_code e;
            if (!fs::create_directories(d, e))
            {
                // For some reason sometimes this is zero (success)?
                if (e.value() != 0)
                {
                    throw FSException(d.string() + " is not a valid directory (error: " + e.message() + ").");
                }
            }
        }

        const int RETRIES = 3;
        const int RETRY_DELAY = 100;

        void assureIsRemoved(const fs::path p)
        {
            if (!fs::exists(p))
                return;

            std::error_code e;
            fs::remove_all(p, e);

            int retries = RETRIES;

            while (e.value() && retries > 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(RETRY_DELAY));
                fs::remove_all(p, e);

                if (!e.value())
                    break;
                retries--;
            }

            if (e.value())
            {
                throw FSException(p.string() + " cannot be removed, error: " + e.message());
            }

            if (fs::exists(p))
            {
                throw FSException(p.string() + " cannot be removed after multiple attempts");
            }
        }

        void copy(const fs::path &from, const fs::path &to)
        {
            std::error_code e;

            fs::copy(from, to, fs::copy_options::overwrite_existing, e);
            if (e.value() != 0)
            {
                throw FSException("Cannot copy " + from.string() + " --> " + to.string() +
                                  " (" + e.message() + ")");
            }
        }

        void hardlink(const fs::path &target, const fs::path &linkName)
        {
            std::error_code e;
            fs::create_hard_link(target, linkName, e);
            if (e.value() != 0)
            {
                throw FSException("Cannot create hard link " + target.string() + " --> " + linkName.string() +
                                  " (" + e.message() + ")");
            }
        }

        void hardlinkSafe(const fs::path &target, const fs::path &linkName)
        {
            if (fs::exists(linkName))
                io::remove(linkName);

            try
            {
                io::hardlink(target, linkName);
            }
            catch (const FSException &)
            {
                LOGD << "Falling back hard link to copy for " + target.string() + " --> " + linkName.string();
                io::copy(target, linkName);
            }
        }

        void remove(const fs::path p)
        {
            std::error_code e;
            fs::remove(p, e);
            if (e.value() != 0)
            {
                throw FSException("Cannot remove " + p.string() + " (" + e.message() + ")");
            }
        }

        bool exists(const fs::path p)
        {
            std::error_code e;
            bool val = fs::exists(p, e);
            if (e.value() != 0)
            {
                return false;
            }
            return val;
        }

        void rename(const fs::path &from, const fs::path &to)
        {
            std::error_code e;
            fs::rename(from, to, e);
            if (e.value() != 0)
            {
                throw FSException("Cannot move " + from.string() + " --> " + to.string() +
                                  " (" + e.message() + ")");
            }
        }

        FileLock::FileLock()
        {
        }

        FileLock::FileLock(const fs::path &p)
        {
            lock(p);
        }

        FileLock::~FileLock()
        {
            unlock();
        }

        void FileLock::lock(const fs::path &p)
        {
            if (!lockFile.empty())
                throw ddb::AppException("lock() already called");
            lockFile = (p.parent_path() / p.filename()).string() + ".lock";

            fd = _open(lockFile.c_str(), O_CREAT, 0644);
            if (fd == -1)
            {
                throw ddb::AppException("Cannot acquire lock " + lockFile);
            }

            // Block
            LOGD << "Acquiring lock " << lockFile;
            flock(fd, LOCK_EX);
        }

        void FileLock::unlock()
        {
            if (fd != -1)
            {
                LOGD << "Freeing lock " + lockFile;

                if (_close(fd) != 0)
                {
                    LOGD << "Cannot close lock " << lockFile;
                }

                if (_unlink(lockFile.c_str()) != 0)
                {
                    LOGD << "Cannot remove lock " << lockFile;
                }

                fd = -1;
            }
        }

#ifdef WIN32
        int flock(int fd, int operation)
        {
            HANDLE h = (HANDLE)_get_osfhandle(fd);

            if (h == (HANDLE)-1)
            {
                errno = EBADF;
                return -1;
            }

            operation &= ~LOCK_NB;

            // We implement just LOCK_EX
            if (LOCK_EX)
            {
                DWORD lower, upper;
                OVERLAPPED ol;

                int flags = LOCKFILE_EXCLUSIVE_LOCK;
                lower = GetFileSize(h, &upper);
                if (lower == INVALID_FILE_SIZE)
                {
                    errno = EBADF;
                    return -1;
                }

                // start offset = 0
                memset(&ol, 0, sizeof ol);

                return LockFileEx(h, flags, 0, lower, upper, &ol);
            }
            else
            {
                errno = EINVAL;
                return -1;
            }
        }
#endif

    }
}
