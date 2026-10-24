/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "mio.h"

#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <thread>

#include <fcntl.h>

#include "exceptions.h"
#include "logger.h"
#include "randomutil.h"
#include "stringutil.h"

#ifdef _WIN32
#include <io.h>  // _wopen, _close, _wunlink, _get_osfhandle
#include <windows.h>
#else
#include <sys/file.h>  // flock
#include <unistd.h>    // close, unlink, readlink
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

namespace ddb
{
    namespace io
    {

        bool isNetworkPath(const std::string &path)
        {
            return path.starts_with("http://") || path.starts_with("https://");
        }

        std::string lowerExtension(const fs::path &p)
        {
            std::string ext = io::toUtf8(p.extension());
            if (ext.empty())
                return ext;
            ext.erase(0, 1);
            str::toLower(ext);
            return ext;
        }

        bool Path::checkExtension(const std::initializer_list<std::string> &matches) const
        {
            const std::string ext = lowerExtension(p);
            if (ext.empty())
                return false;

            for (const auto &m : matches)
                if (str::iequals(m, ext))
                    return true;
            return false;
        }

        namespace
        {
            // A file time as the whole seconds since the Unix epoch (rounded down, also before
            // 1970) and the sub-second part the file system keeps. std::chrono::clock_cast is
            // the portable C++20 spelling of the conversion: MSVC has no file_clock::to_sys.
            struct UnixTime
            {
                std::chrono::seconds seconds;
                double fraction; // [0, 1)
            };

            UnixTime toUnixTime(fs::file_time_type t)
            {
                const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
                const auto whole = std::chrono::floor<std::chrono::seconds>(sys);
                return {whole.time_since_epoch(),
                        std::chrono::duration<double>(sys - whole).count()};
            }

            fs::file_time_type fromUnixTime(std::chrono::seconds seconds, std::chrono::nanoseconds nanoseconds)
            {
                const std::chrono::sys_time<std::chrono::nanoseconds> sys(seconds + nanoseconds);
                // Round down to the file clock's own tick (100 ns on Windows) BEFORE the clock
                // conversion: clock_cast keeps the finer duration, and the 1601 -> 1970 offset
                // (1.16e19 ns) overflows a 64-bit nanosecond count, so the time stored would be
                // garbage and last_write_time would fail with ERROR_INVALID_PARAMETER.
                return std::chrono::clock_cast<fs::file_time_type::clock>(
                    std::chrono::floor<fs::file_time_type::duration>(sys));
            }

            // What a failed file system call says: "<what> <path> (<reason>)"
            [[noreturn]] void throwFsError(const char *what, const fs::path &p, const std::error_code &ec)
            {
                throw FSException(std::string(what) + " " + io::toUtf8(p) + " (" + ec.message() + ")");
            }
        }

        time_t Path::getModifiedTime()
        {
            std::error_code ec;
            const auto mtime = fs::last_write_time(p, ec);
            if (ec)
                throwFsError("Cannot stat mtime", p, ec);
            return static_cast<time_t>(toUnixTime(mtime).seconds.count());
        }

        bool Path::setModifiedTime(time_t mtime)
        {
            if (getModifiedTime() == mtime)
                return false;

            std::error_code ec;
            fs::last_write_time(p, fromUnixTime(std::chrono::seconds(mtime), std::chrono::nanoseconds(0)), ec);
            if (ec)
                throwFsError("Cannot set mtime", p, ec);
            return true;
        }

        FileState Path::getState()
        {
            std::error_code ec;
            const fs::file_status status = fs::status(p, ec);
            if (ec)
                throwFsError("Cannot stat", p, ec);

            FileState state;
            state.directory = status.type() == fs::file_type::directory;
            if (!state.directory)
            {
                state.size = fs::file_size(p, ec);
                if (ec)
                    throwFsError("Cannot stat size", p, ec);
            }

            const auto mtime = fs::last_write_time(p, ec);
            if (ec)
                throwFsError("Cannot stat mtime", p, ec);
            const UnixTime t = toUnixTime(mtime);
            state.mtime = static_cast<double>(t.seconds.count()) + t.fraction;
            return state;
        }

        double Path::getModifiedTimePrecise()
        {
            std::error_code ec;
            const auto mtime = fs::last_write_time(p, ec);
            if (ec)
                throwFsError("Cannot stat mtime", p, ec);
            const UnixTime t = toUnixTime(mtime);
            return static_cast<double>(t.seconds.count()) + t.fraction;
        }

        bool Path::setModifiedTimePrecise(double mtime)
        {
            if (sameModifiedTime(mtime, getModifiedTimePrecise()))
                return false;

            const double seconds = std::floor(mtime);
            const auto nanoseconds = std::min<long long>(std::llround((mtime - seconds) * 1e9), 999999999);

            std::error_code ec;
            fs::last_write_time(p,
                                fromUnixTime(std::chrono::seconds(static_cast<long long>(seconds)),
                                             std::chrono::nanoseconds(nanoseconds)),
                                ec);
            if (ec)
                throwFsError("Cannot set mtime", p, ec);
            return true;
        }

        bool sameModifiedTime(double stored, double file)
        {
            if (stored == std::floor(stored))
                return std::floor(file) == stored;
            return std::abs(stored - file) < 1e-6;
        }

        std::uintmax_t Path::getSize()
        {
            std::error_code ec;
            const auto size = fs::file_size(p, ec);
            if (ec)
                throwFsError("Cannot stat size", p, ec);
            return size;
        }

        namespace
        {
            // Absolute, normalized, without a trailing separator
            std::string normalizedAbsolute(const fs::path &path)
            {
                std::string s = io::toUtf8(fs::weakly_canonical(fs::absolute(path)));
                if (s.length() > 1 && s.back() == fs::path::preferred_separator)
                    s.pop_back();
                return s;
            }

            // Whether normalized `child` is below normalized `parent` (prefixed by it AND followed
            // by a separator: /foo is not a parent of /foobar)
            bool isBelow(const std::string &parent, const std::string &child)
            {
                return child.size() > parent.size() && child.starts_with(parent) &&
                       (parent.back() == fs::path::preferred_separator ||
                        child[parent.size()] == fs::path::preferred_separator);
            }
        }

        bool Path::hasChildren(const std::vector<std::string> &childPaths)
        {
            try
            {
                const std::string absP = normalizedAbsolute(p);

                for (auto &cp : childPaths)
                {
                    const std::string absC = normalizedAbsolute(io::fromUtf8(cp));

                    // A path is contained within itself (e.g. `ddb add .` from project root).
                    if (absC != absP && !isBelow(absP, absC))
                        return false;
                }

                return true;
            }
            catch (const fs::filesystem_error &e)
            {
                LOGD << e.what();
                throw FSException(e.what(), e.code());
            }
        }

        bool Path::isParentOf(const fs::path &childPath)
        {
            try
            {
                return isBelow(normalizedAbsolute(p), normalizedAbsolute(childPath));
            }
            catch (const fs::filesystem_error &e)
            {
                LOGD << e.what();
                throw FSException(e.what(), e.code());
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
                    it != fs::current_path().root_name())
                    count++;
            }
            return std::max(count - 1, 0);
        }

        Path Path::relativeTo(const fs::path &parent)
        {
            try
            {
                const fs::path absPath = weakly_canonical(absolute(p));
                const fs::path absParent = weakly_canonical(absolute(parent));

                // Special case where parent == path
                if (absPath == absParent)
                {
                    return fs::path("");
                }

                // Handle special cases where root is "/"
                // in this case we return the relative canonical absolute path
                if (parent == parent.root_path() || parent == "/")
                {
                    return absPath.relative_path();
                }

                // What fs::relative() returns, without canonicalizing both paths again
                fs::path relPath = absPath.lexically_relative(absParent);
                if (io::toGenericUtf8(relPath) == ".")
                {
                    return absParent;
                }

                return relPath;
            }
            catch (const fs::filesystem_error &e)
            {
                LOGD << e.what();
                throw FSException(e.what(), e.code());
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
            std::string res = io::toGenericUtf8(p);

            // Remove trailing slash from directory (unless "/")
            if (res.length() > 1 && res[res.length() - 1] == '/')
                res.pop_back();

            return res;
        }

        std::string Path::string() const
        {
            return io::toUtf8(p);
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
                return fromUtf8(result).parent_path();
            }
            return fs::path("");
#else
            char result[PATH_MAX];
            ssize_t count = readlink("/proc/self/exe", result, PATH_MAX);
            return fromUtf8(std::string_view(result, static_cast<size_t>((count > 0) ? count : 0))).parent_path();
#endif
        }

        fs::path getDataPath(const fs::path &p)
        {
            // Attempt to find a path within the places we would
            // usually look for DATA files.
            if (const auto ddbDataPath = getEnvUtf8("DDB_DATA"))
            {
                const fs::path candidate = fromUtf8(*ddbDataPath) / p;
                if (fs::exists(candidate))
                    return candidate;
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
            wchar_t dllPath[MAX_PATH];
            HMODULE hm = NULL;

            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCWSTR)&getDataPath, &hm) == 0)
            {
                int ret = GetLastError();
                LOGD << "GetModuleHandle failed, error = " << ret;
                return "";
            }

            if (GetModuleFileNameW(hm, dllPath, MAX_PATH) == 0)
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
            // Not _getcwd: it returns the folder in the ANSI code page on Windows
            std::error_code ec;
            fs::path cwd = fs::current_path(ec);
            if (ec)
                throw FSException("Cannot get cwd");
            return cwd;
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
                        if (*std::next(it->begin(), i) != *std::next(smallest.begin(), i))
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
                throw FSException(io::toUtf8(d) + " is not a valid directory (there might be a file with the same name).");
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
                    throw FSException(io::toUtf8(d) + " is not a valid directory (error: " + e.message() + ").", e);
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
                throw FSException(io::toUtf8(p) + " cannot be removed, error: " + e.message());
            }

            if (fs::exists(p))
            {
                throw FSException(io::toUtf8(p) + " cannot be removed after multiple attempts");
            }
        }

        ScopedTempDir::~ScopedTempDir()
        {
            reset();
        }

        ScopedTempDir::ScopedTempDir(ScopedTempDir &&other) noexcept : path_(other.release())
        {
        }

        ScopedTempDir &ScopedTempDir::operator=(ScopedTempDir &&other) noexcept
        {
            if (this != &other)
                reset(other.release());
            return *this;
        }

        ScopedTempDir ScopedTempDir::create(const fs::path &parent, const std::string &prefix)
        {
            ScopedTempDir dir(parent / (prefix + randomString(16)));
            createDirectories(dir.path());
            return dir;
        }

        void ScopedTempDir::reset(fs::path p) noexcept
        {
            const fs::path old = std::exchange(path_, std::move(p));
            if (old.empty())
                return;
            // Runs during stack unwinding too: never throw
            try
            {
                std::error_code ec;
                if (fs::exists(old, ec))
                {
                    LOGD << "Removing temporary " << io::toUtf8(old);
                    assureIsRemoved(old);
                }
            }
            catch (const std::exception &e)
            {
                LOGW << "Cannot remove temporary " << io::toUtf8(old) << ": " << e.what();
            }
        }

        fs::path ScopedTempDir::release() noexcept
        {
            return std::exchange(path_, fs::path());
        }

        void copy(const fs::path &from, const fs::path &to)
        {
            std::error_code e;

            fs::copy(from, to, fs::copy_options::overwrite_existing, e);
            if (e.value() != 0)
            {
                throw FSException("Cannot copy " + io::toUtf8(from) + " --> " + io::toUtf8(to) +
                                  " (" + e.message() + ")", e);
            }
        }

        void hardlink(const fs::path &target, const fs::path &linkName)
        {
            std::error_code e;
            fs::create_hard_link(target, linkName, e);
            if (e.value() != 0)
            {
                throw FSException("Cannot create hard link " + io::toUtf8(target) + " --> " + io::toUtf8(linkName) +
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
                LOGD << "Falling back hard link to copy for " + io::toUtf8(target) + " --> " + io::toUtf8(linkName);
                io::copy(target, linkName);
            }
        }

        void remove(const fs::path p)
        {
            std::error_code e;
            fs::remove(p, e);
            if (e.value() != 0)
            {
                throw FSException("Cannot remove " + io::toUtf8(p) + " (" + e.message() + ")");
            }
        }

        bool isSamePath(const fs::path &a, const fs::path &b)
        {
            if (a.empty() || b.empty())
                return false;
            std::error_code e;
            if (fs::exists(a, e) && !e && fs::exists(b, e) && !e)
            {
                const bool same = fs::equivalent(a, b, e);
                return same && !e;
            }

            // Lexical form when the file system cannot resolve the path
            const auto canonical = [](const fs::path &p)
            {
                std::error_code e;
                fs::path abs = fs::absolute(p, e);
                if (e)
                    abs = p;
                const fs::path c = fs::weakly_canonical(abs, e);
                return e ? abs.lexically_normal() : c;
            };
            return canonical(a) == canonical(b);
        }

        std::vector<uint8_t> readAllBytes(const fs::path &p)
        {
            std::ifstream in(p, std::ios::binary | std::ios::ate);
            if (!in)
                throw FSException("Cannot open " + io::toUtf8(p));
            const std::streamoff size = in.tellg();
            if (size < 0)
                throw FSException("Cannot read " + io::toUtf8(p));
            std::vector<uint8_t> data(static_cast<size_t>(size));
            in.seekg(0, std::ios::beg);
            if (!in.read(reinterpret_cast<char *>(data.data()), size))
                throw FSException("Cannot read " + io::toUtf8(p));
            return data;
        }

        void assureDistinctOutput(const fs::path &input, const fs::path &output)
        {
            if (isSamePath(input, output))
                throw InvalidArgsException("The output path " + io::toUtf8(output) +
                                           " is the input file " + io::toUtf8(input) +
                                           ": choose a different output path");
        }

        void rename(const fs::path &from, const fs::path &to)
        {
            std::error_code e;
            fs::rename(from, to, e);
            if (e.value() != 0)
            {
                throw FSException("Cannot move " + io::toUtf8(from) + " --> " + io::toUtf8(to) +
                                  " (" + e.message() + ")", e);
            }
        }

        void writeFileAtomic(const fs::path &target, const std::string &content, const std::string &what,
                             bool ownerOnly, const std::string &tempTag)
        {
            const fs::path temporary = concat(target, tempTag + randomString(16));
            try
            {
                std::ofstream out;
                errno = 0;
                out.open(temporary, std::ios::binary | std::ios::trunc);
                const int openError = errno;
                if (!out.is_open())
                    throw FSException("Cannot open " + what + " " + toUtf8(temporary),
                                      std::error_code(openError, std::generic_category()));
#ifndef WIN32
                if (ownerOnly)
                {
                    // Still empty: nothing readable by others exists at any time
                    std::error_code permError;
                    fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write,
                                    fs::perm_options::replace, permError);
                    if (permError)
                        throw FSException("Cannot restrict " + what + " " + toUtf8(temporary) + " to its owner (" +
                                              permError.message() + ")",
                                          permError);
                }
#else
                (void)ownerOnly;
#endif
                errno = 0;
                out << content;
                out.flush();
                const int writeError = errno;
                if (!out)
                    throw FSException("Cannot write " + what + " " + toUtf8(temporary),
                                      std::error_code(writeError, std::generic_category()));
                errno = 0;
                out.close();
                const int closeError = errno;
                if (out.fail())
                    throw FSException("Cannot close " + what + " " + toUtf8(temporary),
                                      std::error_code(closeError, std::generic_category()));
#ifdef WIN32
                // std::filesystem::rename does not replace an existing file on Windows.
                if (!MoveFileExW(temporary.c_str(), target.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                {
                    const std::error_code ec(GetLastError(), std::system_category());
                    throw FSException("Cannot replace " + what + ": " + ec.message(), ec);
                }
#else
                io::rename(temporary, target);
#endif
            }
            catch (...)
            {
                std::error_code ignored;
                fs::remove(temporary, ignored);
                throw;
            }
        }

        bool moveOrCopy(const fs::path &from, const fs::path &to)
        {
            std::error_code e;
            const auto links = fs::hard_link_count(from, e);
            if (!e && links == 1)
            {
                fs::rename(from, to, e);
                if (!e)
                    return true;
                LOGD << "Cannot rename " << io::toUtf8(from) << " --> " << io::toUtf8(to) << " (" << e.message()
                     << "), copying it";
            }

            io::copy(from, to);
            io::remove(from);
            return false;
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

        namespace
        {
#ifdef _WIN32
            // Blocks until this process holds an exclusive lock on the whole file (flock(LOCK_EX))
            int lockExclusive(int fd)
            {
                HANDLE h = (HANDLE)_get_osfhandle(fd);

                if (h == (HANDLE)-1)
                {
                    errno = EBADF;
                    return -1;
                }

                DWORD lower, upper;
                OVERLAPPED ol;

                lower = GetFileSize(h, &upper);
                if (lower == INVALID_FILE_SIZE)
                {
                    errno = EBADF;
                    return -1;
                }

                // start offset = 0
                memset(&ol, 0, sizeof ol);

                return LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK, 0, lower, upper, &ol);
            }

            int openLockFile(const std::string &utf8Path)
            {
                return _wopen(toWide(utf8Path).c_str(), O_CREAT, 0644);
            }

            int closeLockFile(int fd) { return _close(fd); }

            int removeLockFile(const std::string &utf8Path)
            {
                return _wunlink(toWide(utf8Path).c_str());
            }
#else
            int lockExclusive(int fd) { return ::flock(fd, LOCK_EX); }

            int openLockFile(const std::string &utf8Path) { return ::open(utf8Path.c_str(), O_CREAT, 0644); }

            int closeLockFile(int fd) { return ::close(fd); }

            int removeLockFile(const std::string &utf8Path) { return ::unlink(utf8Path.c_str()); }
#endif
        }

        void FileLock::lock(const fs::path &p)
        {
            if (!lockFile.empty())
                throw ddb::AppException("lock() already called");
            lockFile = io::toUtf8(p.parent_path() / p.filename()) + ".lock"; // UTF-8

            fd = openLockFile(lockFile);
            if (fd == -1)
            {
                throw ddb::AppException("Cannot acquire lock " + lockFile);
            }

            // Block
            LOGD << "Acquiring lock " << lockFile;
            lockExclusive(fd);
        }

        void FileLock::unlock()
        {
            if (fd != -1)
            {
                LOGD << "Freeing lock " + lockFile;

                if (closeLockFile(fd) != 0)
                {
                    LOGD << "Cannot close lock " << lockFile;
                }

                if (removeLockFile(lockFile) != 0)
                {
                    LOGD << "Cannot remove lock " << lockFile;
                }

                fd = -1;
            }
        }

    }
}
