/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef BUILD_H
#define BUILD_H

#include "dbops.h"

#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace ddb
{

    /**
     * The output subfolder of the build of the entry at `path` ("<build root>/<hash>/<subfolder>"),
     * or nothing when the entry is not buildable, neither itself nor as the sidecar of an indexed
     * main file. Throws InvalidArgsException when `path` is not indexed.
     */
    std::optional<std::string> buildSubfolder(Database *db, const std::string &path);

    /// buildSubfolder() has a value; throws InvalidArgsException when `path` is not indexed
    bool isBuildable(Database *db, const std::string &path);

    /**
     * The indexed main files the sidecar at path belongs to (the SidecarRules of the entry
     * handlers): .cpg/.dbf/.shx belong to a shapefile; a .prj also gives a CAD drawing its CRS
     * (DDB-12). A main file has the sidecar's folder and stem (exact bytes) and a main
     * extension in any letter case (.shp, .SHP, .Shp). Lookup order: the extensions in rule
     * order, then lowercase, uppercase and other spellings, then byte order of the path.
     * Empty for any other file.
     */
    std::vector<std::string> findSidecarMains(Database *db, const std::string &path);

    void buildAll(Database *db, const std::string &outputPath, bool force = false);
    void build(Database *db, const std::string &path, const std::string &outputPath, bool force = false);

    /**
     * @brief Retry every deferred build (one per "<hash>.pending" marker in the build
     * directory). A retry is a full build attempt of the entry: the dependency names in
     * the marker are informational and are not matched against the index. Entries
     * attempted less than 60 seconds ago are skipped unless @p force is set. Errors are
     * logged, never thrown: missing dependencies refresh the marker. Low disk space,
     * identifiable ENOSPC and primary-tool idle timeout preserve an existing marker;
     * permanent failures and complete builds consume it. Lock contention leaves it
     * untouched. Fresh failed builds do not gain a marker except for dependencies.
     * @param db database (already opened)
     * @param outputPath base build directory; if empty, uses db->buildDirectory()
     * @param force rebuild even if complete, and ignore the retry backoff
     */
    void buildPending(Database *db, const std::string &outputPath, bool force = false);
    bool isBuildPending(Database *db);

    /**
     * @brief Information about a single deferred build, read from a ".pending"
     * marker file. Missing dependencies are companion/sidecar file paths,
     * relative to the dataset root (e.g. "sub/m.mtl" or "sub/layer.shx");
     * markers written by older versions may hold names relative to the
     * source file's folder. Missing external tools (e.g. "build-lod") are not
     * deferred: they fail the build. A marker retained after a classified primary
     * transient failure keeps the dependency hints from its prior deferred attempt.
     */
    struct PendingBuildInfo
    {
        /// Entry hash (matches the ".pending" file stem)
        std::string hash;
        /// Entry path resolved from the database ("" if the entry no longer exists)
        std::string path;
        /// Dependency names still missing at the time of the last attempt
        std::vector<std::string> missingDependencies;
        /// Unix timestamp of the last build attempt (0 if unknown/malformed)
        time_t lastAttempt = 0;
        /// First Unix second at which buildPending() (without force) retries this build:
        /// lastAttempt + the retry backoff (60 s). Due when the current time is at or past it.
        time_t retryAfter = 0;
    };

    /**
     * @brief Read-only view over all pending (deferred) builds in a database.
     * Unlike buildPending(), this does not consume ".pending" files, does not
     * apply the retry backoff, and never attempts a build. Entries
     * whose hash no longer matches any indexed entry are skipped (they are
     * garbage collected by cleanupBuild()).
     * @param db database (already opened)
     * @return list of pending build info, one per ".pending" file with a
     *         matching indexed entry
     */
    std::vector<PendingBuildInfo> getPendingBuildInfo(Database *db);

    /// {"hash", "path", "missingDependencies", "lastAttempt", "retryAfter"}: an element of the
    /// output of DDBGetPendingBuildInfo (tests/contract/schemas/pending-build-info.schema.json)
    void to_json(json &j, const PendingBuildInfo &info);

    bool isBuildActive(Database *db, const std::string &path);

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
    bool isBuildComplete(Database *db, const std::string &path);

    /**
     * @brief Build status of one entry, as reported by getBuildStatus().
     */
    struct BuildStatus
    {
        /// Entry path, as requested
        std::string path;
        /// The index has an entry with this path
        bool exists = false;
        /// Same answer as isBuildable() (false when the entry does not exist)
        bool buildable = false;
        /// Same answer as isBuildComplete()
        bool complete = false;
    };

    /**
     * @brief isBuildable() and isBuildComplete() for several entries on one connection.
     * Reads only path, hash and type of each entry; the completeness checks are the
     * same filesystem checks as isBuildComplete().
     * @param db database handle
     * @param paths dataset-relative entry paths (exact match)
     * @return one status per input path, in input order
     */
    std::vector<BuildStatus> getBuildStatus(Database *db, const std::vector<std::string> &paths);

    /// {"path", "exists", "buildable", "complete"}: an element of the output of DDBGetBuildStatus
    void to_json(json &j, const BuildStatus &status);

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
    CleanupResult cleanupBuild(Database *db, const std::string &outputPath = "");

    /// {"entries": removedEntries, "builds": removedBuilds}: the output of DDBCleanup
    void to_json(json &j, const CleanupResult &result);

}

#endif // BUILD_H
