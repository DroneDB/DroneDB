/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef BUILD_H
#define BUILD_H

#include "ddb_export.h"
#include "dbops.h"

namespace ddb
{

    DDB_DLL bool isBuildable(Database *db, const std::string &path, std::string &subfolder);

    DDB_DLL void buildAll(Database *db, const std::string &outputPath, bool force = false);
    DDB_DLL void build(Database *db, const std::string &path, const std::string &outputPath, bool force = false);

    DDB_DLL void buildPending(Database *db, const std::string &outputPath, bool force = false);
    DDB_DLL bool isBuildPending(Database *db);

    DDB_DLL bool isBuildActive(Database *db, const std::string &path);

    /**
     * @brief Check whether the build artifacts for an entry are present and non-empty.
     *
     * Returns true only when the entry is buildable AND the expected output files
     * exist and have non-zero size. Validates content rather than mere folder
     * existence so that interrupted or aborted builds (which can leave empty
     * directories behind) are reported as incomplete and re-queued.
     *
     * @param db database handle
     * @param path entry path
     * @return true if the build output is complete and valid; false otherwise
     *         (not buildable, missing, or empty artifacts)
     */
    DDB_DLL bool isBuildComplete(Database *db, const std::string &path);

    /**
     * @brief Result of a cleanup operation.
     */
    struct CleanupResult
    {
        /// Entry paths that were removed from the database because their
        /// underlying files no longer exist on the filesystem.
        std::vector<std::string> removedEntries;
        /// Filesystem paths of orphaned build artifacts (subdirectories or
        /// .pending files) that were removed from the build directory.
        std::vector<std::string> removedBuilds;
    };

    /**
     * @brief Perform a cleanup of a dataset:
     *   1. Remove from the database all non-directory entries whose underlying
     *      file no longer exists on the filesystem (their associated build
     *      artifacts are removed by removeFromIndex).
     *   2. Remove orphaned build artifacts (subdirectories and .pending files
     *      whose hash no longer corresponds to any entry in the database).
     *      Subdirectories with an active (live-PID) build lock are skipped.
     *
     * @param db database (already opened)
     * @param outputPath base build directory; if empty, uses db->buildDirectory()
     * @return CleanupResult with the lists of removed items
     */
    DDB_DLL CleanupResult cleanupBuild(Database *db, const std::string &outputPath = "");

}

#endif // BUILD_H
