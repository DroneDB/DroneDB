/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef BUILDLOCK_H
#define BUILDLOCK_H

#include <string>
#include "ddb_export.h"

#ifdef WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace ddb {

/**
 * @brief Cross-platform inter-process lock for build operations
 *
 * BuildLock provides a robust mechanism to prevent race conditions when multiple
 * processes attempt to build the same file simultaneously. It leverages the atomic
 * nature of filesystem operations to create exclusive locks that work across processes
 * and even across different machines when using network filesystems.
 *
 * @details
 * The lock is implemented using platform-specific atomic file operations:
 * - Windows: CreateFile with exclusive access and FILE_FLAG_DELETE_ON_CLOSE
 * - Unix/Linux: open() with O_CREAT | O_EXCL flags
 *
 * Key features:
 * - Atomic lock acquisition (no race conditions)
 * - Cross-platform compatibility (Windows, Linux, macOS)
 * - Automatic cleanup on process termination
 * - Works with network filesystems (NFS, SMB)
 * - Minimal performance overhead
 * - RAII-style resource management
 *
 * @example
 * @code
 * try {
 *     BuildLock lock("/path/to/output/folder");
 *     // Build operations here - guaranteed exclusive access
 *     performBuild();
 * } catch (const AppException& e) {
 *     if (e.what() == "Build in progress by another process") {
 *         // Handle concurrent build attempt
 *     }
 * }
 * // Lock automatically released when destructor is called
 * @endcode
 *
 * @note
 * The lock file created is named "{outputPath}.building" and contains the PID
 * of the process holding the lock for debugging purposes.
 *
 * @warning
 * This class is designed for use with DroneDB build operations and should not
 * be used as a general-purpose file locking mechanism.
 *
 * @thread_safety
 * This class is not thread-safe. Multiple threads within the same process
 * should coordinate using ThreadLock before attempting to acquire a BuildLock.
 */
class DDB_DLL BuildLock {
private:
    std::string lockFilePath;  ///< Full path to the lock file
    bool isLocked;             ///< Track lock state for proper cleanup

#ifdef WIN32
    HANDLE fileHandle;         ///< Windows file handle for the lock
#else
    int fileDescriptor;        ///< Unix file descriptor for the lock
#endif

    /**
     * @brief Write diagnostic information to the lock file
     *
     * Writes the process ID and optional metadata to the lock file.
     * This information can be useful for debugging deadlocks or
     * identifying which process is holding the lock.
     */
    void writeLockInfo();

    /**
     * @brief Clean up platform-specific resources
     *
     * Closes file handles/descriptors and removes the lock file.
     * Called by the destructor but also usable for early release.
     */
    void cleanup();

    /**
     * @brief Get current timestamp as string for lock diagnostics
     *
     * @return std::string Current timestamp in "YYYY-MM-DD HH:MM:SS" format
     */
    std::string getCurrentTimestamp();

    /**
     * @brief Acquire the build lock with specified wait behavior
     *
     * @param waitForLock If true, use blocking behavior (CREATE_ALWAYS on Windows).
     *                    If false, fail immediately if lock exists (CREATE_NEW on Windows)
     *
     * @throws AppException If the lock cannot be acquired
     */
    void acquireLock(bool waitForLock);

public:
    /**
     * @brief Construct a BuildLock for the specified output path
     *
     * Creates an exclusive lock to prevent other processes from building
     * to the same output location. The lock file is created as
     * "{outputPath}.building".
     *
     * @param outputPath The target build output path to lock
     *
     * @throws AppException If another process is already building to this path
     * @throws AppException If the lock cannot be acquired due to system errors
     *
     * @details
     * The constructor immediately attempts to acquire the lock. If successful,
     * the object is ready to use. If another process already holds the lock,
     * an exception is thrown with a descriptive message.
     *
     * Common failure scenarios:
     * - Another process is building the same file (ERROR_SHARING_VIOLATION on Windows, EEXIST on Unix)
     * - Insufficient permissions to create lock file
     * - Disk full or other I/O errors
     * - Network filesystem temporarily unavailable
     */
    explicit BuildLock(const std::string& outputPath);

    /**
     * @brief Construct a BuildLock with wait option
     *
     * Creates an exclusive lock to prevent other processes from building
     * to the same output location. The lock file is created as
     * "{outputPath}.building".
     *
     * @param outputPath The target build output path to lock
     * @param wait If true, wait for the lock to become available. If false, fail immediately if lock is held
     *
     * @throws AppException If wait=false and another process is already building to this path
     * @throws AppException If the lock cannot be acquired due to system errors
     */
    explicit BuildLock(const std::string& outputPath, bool wait);

    /**
     * @brief Destructor - automatically releases the lock
     *
     * Ensures the lock is properly released and the lock file is removed
     * when the BuildLock object goes out of scope. This provides RAII-style
     * resource management and prevents lock leaks even if exceptions occur.
     *
     * @note
     * On Windows, the lock file is automatically deleted by the system when
     * the process terminates due to FILE_FLAG_DELETE_ON_CLOSE. On Unix systems,
     * the destructor explicitly removes the file.
     */
    ~BuildLock();

    /**
     * @brief Check if this instance currently holds the lock
     *
     * @return true if the lock is currently held by this instance
     * @return false if the lock has been released or was never acquired
     */
    bool isHolding() const { return isLocked; }

    /**
     * @brief Get the path of the lock file
     *
     * @return const std::string& The full path to the lock file
     *
     * @note
     * This can be useful for debugging or logging purposes to identify
     * which specific lock file is being used.
     */
    const std::string& getLockFilePath() const { return lockFilePath; }

    /**
     * @brief Manually release the lock before destructor
     *
     * Explicitly releases the lock and cleans up resources. This can be called
     * multiple times safely. After calling this method, isHolding() will return false.
     *
     * @note
     * This is primarily useful when you want to release the lock early,
     * before the BuildLock object goes out of scope.
     */
    void release();

    // Disable copy constructor and assignment operator
    // BuildLock should not be copied as it manages an exclusive resource
    BuildLock(const BuildLock&) = delete;
    BuildLock& operator=(const BuildLock&) = delete;

    // Allow move constructor and assignment for efficiency
    BuildLock(BuildLock&& other) noexcept;
    BuildLock& operator=(BuildLock&& other) noexcept;
};

} // namespace ddb

#endif // BUILDLOCK_H