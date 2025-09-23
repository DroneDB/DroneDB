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
        // Close the file descriptor
        if (close(fileDescriptor) == -1) {
            LOGW << "Failed to close lock file descriptor: " << strerror(errno);
        }
        fileDescriptor = -1;

        // Manually remove the lock file on Unix systems
        if (unlink(lockFilePath.c_str()) == -1) {
            // Only log if the error is not "file not found" (already deleted)
            if (errno != ENOENT) {
                LOGW << "Failed to remove lock file " << lockFilePath << ": " << strerror(errno);
            }
        }
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

    // Create parent directory if it doesn't exist
    // This ensures that lock acquisition won't fail due to missing directories
    std::string parentDir = lockFilePath.substr(0, lockFilePath.find_last_of("/\\"));
    if (!parentDir.empty()) {
        LOGD << "Ensuring parent directory exists: " << parentDir;
        try {
            fs::create_directories(parentDir);
        } catch (const std::exception& e) {
            LOGW << "Failed to create parent directory " << parentDir << ": " << e.what();
            // Continue anyway - the lock operation itself will catch directory errors
        }
    }

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
    // Unix implementation using open() with O_EXCL for atomic creation
    // O_EXCL ensures that open() fails if the file already exists
    fileDescriptor = open(
        lockFilePath.c_str(),
        O_CREAT | O_EXCL | O_WRONLY,  // Create exclusively, write-only
        S_IRUSR | S_IWUSR | S_IRGRP   // Permissions: rw-r-----
    );

    if (fileDescriptor == -1) {
        int error = errno;

        if (error == EEXIST) {
            // File already exists - another process is building
            throw BuildInProgressException("Build in progress by another process");
        } else if (error == EACCES) {
            throw BuildLockPermissionException("Insufficient permissions to create build lock file: " + lockFilePath);
        } else if (error == ENOSPC) {
            throw BuildLockDiskFullException("Disk full - cannot create build lock file: " + lockFilePath);
        } else if (error == ENOENT) {
            throw BuildLockDirectoryException("Lock file directory does not exist: " + lockFilePath);
        } else if (error == ENAMETOOLONG) {
            throw BuildLockException("Lock file path too long: " + lockFilePath);
        } else {
            // Generic error with errno for debugging
            std::ostringstream ss;
            ss << "Failed to acquire build lock (" << strerror(error) << "): " << lockFilePath;
            throw BuildLockException(ss.str());
        }
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