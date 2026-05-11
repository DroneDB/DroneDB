/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "buildlock.h"
#include "exceptions.h"
#include "logger.h"
#include "fs.h"

#include <sstream>
#include <fstream>
#include <chrono>
#include <iomanip>

#ifdef WIN32
#include <process.h>  // for _getpid()
#else
#include <sys/stat.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <unistd.h>   // for getpid()
#  if defined(__linux__) && defined(F_OFD_SETLK)
     // Linux 3.15+: prefer Open File Description locks - per-fd semantics,
     // immune to the F_SETLK "any close releases all locks" foot-gun,
     // and NFSv4-safe.
#    define DDB_USE_OFD_LOCKS 1
#  else
     // macOS / BSD / older Linux: fall back to flock() which is also per-fd
     // (in modern kernels) and supported on all common Unix-like systems.
#    include <sys/file.h>
#    define DDB_USE_FLOCK 1
#  endif
#endif

// --- File-private helpers for Unix kernel locking -------------------------
#ifndef WIN32
namespace {

/// Attempt to acquire an exclusive advisory lock on the open file descriptor.
/// Non-blocking: returns true if the lock is acquired, false if another process
/// already holds it. Any other error is reported via `outErrno` and returns false.
bool tryAcquireKernelLock(int fd, int* outErrno) {
    *outErrno = 0;
#  ifdef DDB_USE_OFD_LOCKS
    struct flock fl{};
    fl.l_type   = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start  = 0;
    fl.l_len    = 0;  // whole file
    if (fcntl(fd, F_OFD_SETLK, &fl) == 0) return true;
#  else
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) return true;
#  endif
    *outErrno = errno;
    return false;
}

/// Release the advisory lock. Best-effort; close() also releases automatically.
void releaseKernelLock(int fd) {
#  ifdef DDB_USE_OFD_LOCKS
    struct flock fl{};
    fl.l_type   = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start  = 0;
    fl.l_len    = 0;
    (void)fcntl(fd, F_OFD_SETLK, &fl);
#  else
    (void)flock(fd, LOCK_UN);
#  endif
}

} // anonymous namespace
#endif

namespace ddb {

BuildLock::BuildLock(const std::string& outputPath)
    : lockFilePath(outputPath + ".building")
    , isLocked(false)
#ifdef WIN32
    , fileHandle(INVALID_HANDLE_VALUE)
#else
    , fileDescriptor(-1)
#endif
{
    acquireLock(true);  // Use waiting behavior (original constructor behavior)
}

BuildLock::BuildLock(const std::string& outputPath, bool wait)
    : lockFilePath(outputPath + ".building")
    , isLocked(false)
#ifdef WIN32
    , fileHandle(INVALID_HANDLE_VALUE)
#else
    , fileDescriptor(-1)
#endif
{
    acquireLock(wait);  // Use the common logic with the wait parameter
}

BuildLock::~BuildLock() {
    if (isLocked) {
        LOGD << "Releasing build lock in destructor: " << lockFilePath;
        cleanup();
    }
}

void BuildLock::writeLockInfo() {
    if (!isLocked) return;

    try {
#ifdef WIN32
        // Write PID and additional info to the lock file on Windows
        DWORD pid = _getpid();
        std::ostringstream info;
        info << "PID: " << pid << "\n";
        info << "Process: DroneDB Build\n";
        info << "Lock created: " << getCurrentTimestamp() << "\n";

        std::string infoStr = info.str();
        DWORD bytesWritten;

        if (!WriteFile(fileHandle, infoStr.c_str(), static_cast<DWORD>(infoStr.length()),
                      &bytesWritten, nullptr)) {
            LOGW << "Failed to write lock info to file (Windows error " << GetLastError() << ")";
        } else {
            FlushFileBuffers(fileHandle);  // Ensure data is written to disk
        }
#else
        // Write PID and additional info to the lock file on Unix
        pid_t pid = getpid();
        std::ostringstream info;
        info << "PID: " << pid << "\n";
        info << "Process: DroneDB Build\n";
        info << "Lock created: " << getCurrentTimestamp() << "\n";

        std::string infoStr = info.str();
        ssize_t bytesWritten = write(fileDescriptor, infoStr.c_str(), infoStr.length());

        if (bytesWritten == -1) {
            LOGW << "Failed to write lock info to file: " << strerror(errno);
        } else {
            fsync(fileDescriptor);  // Ensure data is written to disk
        }
#endif
    } catch (const std::exception& e) {
        // Non-fatal - just log the error
        LOGW << "Exception while writing lock info: " << e.what();
    }
}

void BuildLock::cleanup() {
    if (!isLocked) return;

    LOGD << "Cleaning up build lock: " << lockFilePath;

#ifdef WIN32
    if (fileHandle != INVALID_HANDLE_VALUE) {
        // Close the file handle - FILE_FLAG_DELETE_ON_CLOSE ensures file deletion
        if (!CloseHandle(fileHandle)) {
            LOGW << "Failed to close lock file handle (Windows error " << GetLastError() << ")";
        }
        fileHandle = INVALID_HANDLE_VALUE;
    }
    // Note: File is automatically deleted by Windows due to FILE_FLAG_DELETE_ON_CLOSE
#else
    if (fileDescriptor != -1) {
        // Order matters: unlink the directory entry BEFORE releasing the kernel
        // lock. If we released the lock first, a competing process could open
        // the file, take the lock on the same inode, and then we'd unlink it -
        // a subsequent third process opening the same path would create a new
        // inode and acquire an independent lock, defeating mutual exclusion.
        //
        // With unlink first: any concurrent open() sees a missing path and
        // creates a fresh inode whose lock is unrelated to ours. POSIX permits
        // unlink() on an open file; the old inode lives on until our close().
        if (unlink(lockFilePath.c_str()) == -1) {
            // Only log if the error is not "file not found" (already deleted)
            if (errno != ENOENT) {
                LOGW << "Failed to remove lock file " << lockFilePath << ": " << strerror(errno);
            }
        }

        // Explicit unlock for clarity. close() would also release the lock.
        releaseKernelLock(fileDescriptor);

        if (close(fileDescriptor) == -1) {
            LOGW << "Failed to close lock file descriptor: " << strerror(errno);
        }
        fileDescriptor = -1;
    }
#endif

    isLocked = false;
    LOGD << "Build lock cleanup completed: " << lockFilePath;
}

void BuildLock::release() {
    if (isLocked) {
        LOGD << "Manually releasing build lock: " << lockFilePath;
        cleanup();
    }
}

// Move constructor
BuildLock::BuildLock(BuildLock&& other) noexcept
    : lockFilePath(std::move(other.lockFilePath))
    , isLocked(other.isLocked)
#ifdef WIN32
    , fileHandle(other.fileHandle)
#else
    , fileDescriptor(other.fileDescriptor)
#endif
{
    // Reset the source object to prevent double cleanup
    other.isLocked = false;
#ifdef WIN32
    other.fileHandle = INVALID_HANDLE_VALUE;
#else
    other.fileDescriptor = -1;
#endif
}

// Move assignment operator
BuildLock& BuildLock::operator=(BuildLock&& other) noexcept {
    if (this != &other) {
        // Clean up current lock if any
        if (isLocked) {
            cleanup();
        }

        // Move data from other object
        lockFilePath = std::move(other.lockFilePath);
        isLocked = other.isLocked;
#ifdef WIN32
        fileHandle = other.fileHandle;
        other.fileHandle = INVALID_HANDLE_VALUE;
#else
        fileDescriptor = other.fileDescriptor;
        other.fileDescriptor = -1;
#endif

        // Reset the source object
        other.isLocked = false;
    }
    return *this;
}

void BuildLock::acquireLock(bool waitForLock) {
    LOGD << "Attempting to acquire build lock" << (waitForLock ? "" : " (no wait)") << ": " << lockFilePath;

    // Check if containing folder exists
    fs::path lockPath(lockFilePath);
    if (!fs::exists(lockPath.parent_path()))
        throw BuildLockDirectoryException("Lock file directory does not exist: " + lockPath.parent_path().string());


#ifdef WIN32
    // Windows implementation using CreateFile with exclusive access
    // FILE_FLAG_DELETE_ON_CLOSE ensures automatic cleanup even if process crashes
    fileHandle = CreateFileA(
        lockFilePath.c_str(),
        GENERIC_WRITE,              // We need write access to write PID info
        0,                          // No sharing - this is the key for exclusivity
        nullptr,                    // Default security attributes
        waitForLock ? CREATE_ALWAYS : CREATE_NEW,  // CREATE_ALWAYS overwrites, CREATE_NEW fails if exists
        FILE_ATTRIBUTE_TEMPORARY |  // Hint that this is a temporary file
        FILE_FLAG_DELETE_ON_CLOSE,  // Automatic cleanup on process termination
        nullptr                     // No template file
    );

    if (fileHandle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();

        if (error == ERROR_SHARING_VIOLATION || error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
            // This is the expected error when another process holds the lock
            throw BuildInProgressException("Build in progress by another process");
        } else if (error == ERROR_ACCESS_DENIED) {
            throw BuildLockPermissionException("Insufficient permissions to create build lock file: " + lockFilePath);
        } else if (error == ERROR_DISK_FULL) {
            throw BuildLockDiskFullException("Disk full - cannot create build lock file: " + lockFilePath);
        } else if (error == ERROR_PATH_NOT_FOUND) {
            throw BuildLockDirectoryException("Lock file directory does not exist: " + lockFilePath);
        } else {
            // Generic error with Windows error code for debugging
            std::ostringstream ss;
            ss << "Failed to acquire build lock (Windows error " << error << "): " << lockFilePath;
            throw BuildLockException(ss.str());
        }
    }

    isLocked = true;
    LOGD << "Build lock acquired successfully" << (waitForLock ? "" : " (no wait)") << ": " << lockFilePath;

#else
    // Unix implementation using kernel-managed advisory locks.
    //
    // Rationale: the previous implementation used O_CREAT|O_EXCL which relied
    // on the lock file's presence to signal "build in progress". That approach
    // is fragile in two ways:
    //   1. If a process crashes, the orphan file remains and requires PID-based
    //      liveness checks to detect staleness. PID checks are unreliable in
    //      Docker containers (PID namespaces restart from low values and the
    //      same PID can belong to an unrelated process after restart).
    //   2. Cross-platform asymmetry: Windows' FILE_FLAG_DELETE_ON_CLOSE makes
    //      orphans impossible, but the Unix branch had no equivalent.
    //
    // The new model: the file is just a container; mutual exclusion is provided
    // by an OS-level advisory lock (F_OFD_SETLK on Linux, flock() elsewhere)
    // that the kernel releases automatically when the holding process dies.
    // Orphan files are harmless: the next process opens them, takes the lock,
    // truncates the body and rewrites the diagnostic info.
    //
    // `waitForLock` is intentionally ignored on Unix (both modes are non-
    // blocking) to preserve the existing higher-level semantics in build.cpp.

    fileDescriptor = open(
        lockFilePath.c_str(),
        O_CREAT | O_WRONLY,           // no O_EXCL: an orphan file is reclaimable
        S_IRUSR | S_IWUSR | S_IRGRP   // permissions: rw-r-----
    );
    (void)waitForLock;  // unused on Unix; preserved in signature for API parity

    if (fileDescriptor == -1) {
        int error = errno;

        if (error == EACCES) {
            throw BuildLockPermissionException("Insufficient permissions to create build lock file: " + lockFilePath);
        } else if (error == ENOSPC) {
            throw BuildLockDiskFullException("Disk full - cannot create build lock file: " + lockFilePath);
        } else if (error == ENOENT) {
            throw BuildLockDirectoryException("Lock file directory does not exist: " + lockFilePath);
        } else if (error == ENAMETOOLONG) {
            throw BuildLockException("Lock file path too long: " + lockFilePath);
        } else {
            std::ostringstream ss;
            ss << "Failed to open build lock file (" << strerror(error) << "): " << lockFilePath;
            throw BuildLockException(ss.str());
        }
    }

    int lockErrno = 0;
    if (!tryAcquireKernelLock(fileDescriptor, &lockErrno)) {
        // Clean up the fd before throwing; do NOT unlink, the lock holder
        // legitimately owns the file.
        close(fileDescriptor);
        fileDescriptor = -1;

        if (lockErrno == EWOULDBLOCK || lockErrno == EAGAIN || lockErrno == EACCES) {
            // EACCES is what F_OFD_SETLK can return when another process holds
            // a conflicting lock on some filesystems (POSIX permits either).
            throw BuildInProgressException("Build in progress by another process");
        }

        std::ostringstream ss;
        ss << "Failed to acquire build lock (" << strerror(lockErrno) << "): " << lockFilePath;
        throw BuildLockException(ss.str());
    }

    // Lock acquired. The file may contain stale diagnostic data from a previous
    // crashed run; truncate it before writing fresh info.
    if (ftruncate(fileDescriptor, 0) == -1) {
        // Non-fatal; lock is valid even if truncate fails.
        LOGW << "Failed to truncate lock file " << lockFilePath << ": " << strerror(errno);
    }

    isLocked = true;
    LOGD << "Build lock acquired successfully" << (waitForLock ? "" : " (no wait)") << ": " << lockFilePath;

#endif

    // Write diagnostic information to the lock file
    try {
        writeLockInfo();
    } catch (const std::exception& e) {
        // Non-fatal error - lock is still valid even if we can't write info
        LOGW << "Failed to write lock info to " << lockFilePath << ": " << e.what();
    }
}

// Helper function to get current timestamp for lock info
std::string BuildLock::getCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);

    std::tm tm = {};
#ifdef WIN32
    // Use localtime_s for thread safety on Windows
    if (localtime_s(&tm, &time_t) != 0)
        return "unknown";
#else
    // Use localtime_r for thread safety on Unix
    if (localtime_r(&time_t, &tm) == nullptr)
        return "unknown";

#endif

    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

} // namespace ddb