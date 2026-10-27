/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef BUILD_INTERNAL_H
#define BUILD_INTERNAL_H

// Private to the build implementation (build.cpp, build_dispatch.cpp,
// pending_markers.cpp, build_cleanup.cpp): not installed, not part of the API.

#include <chrono>
#include <ctime>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "build_manifest.h"
#include "dbops.h"
#include "entry_handler.h"
#include "fs.h"
#include "statement.h"

namespace ddb {

// Reads an indexed entry by path; nothing when there is none.
using EntryLookup = std::function<std::optional<Entry>(const std::string&)>;
// The indexed paths that start with a prefix (byte-wise), in any order.
using PrefixLookup = std::function<std::vector<std::string>(const std::string&)>;

// ---- Build rules (build.cpp) ----------------------------------------------

// What a build of an entry produces, and where.
struct BuildTarget {
    // The entry whose artifacts are built: the entry itself, or the main file a
    // sidecar belongs to (the .shp of a .dbf). Type dispatch, the input path and
    // the output hash all come from it.
    Entry entry;
    // Output subfolder in "<build root>/<entry.hash>/" (handler->subfolder())
    std::string subfolder;
    // The registered handler of entry.type: builds it and checks its completeness
    const EntryHandler* handler = nullptr;
    // The requested entry is a sidecar of `entry` (its build is forced)
    bool viaDependency = false;
};

// The indexed main files of the sidecar at path, in lookup order (see findSidecarMains
// in build.h); siblings lists the indexed paths with a prefix.
std::vector<std::string> findSidecarMains(const std::string& path, const PrefixLookup& siblings);

// The build target of an entry, or nothing when neither the entry nor a main
// file it is a sidecar of is buildable. siblings finds the main files, lookup
// reads them.
std::optional<BuildTarget> resolveBuildTarget(const Entry& e, const EntryLookup& lookup,
                                              const PrefixLookup& siblings);
// Same, reading the index of db.
std::optional<BuildTarget> resolveBuildTarget(Database* db, const Entry& e);

// The indexed paths of db that start with prefix (one range scan of the path index)
std::vector<std::string> indexedPathsWithPrefix(Database* db, const std::string& prefix);

// The build targets of many entries of one index, with two prepared statements reused for
// every lookup (getBuildStatus, the build manifest): only path, hash and type are read.
class IndexBuildTargets {
  public:
    explicit IndexBuildTargets(Database* db);
    // Reads path, hash and type of the entry at path; nothing when there is none
    std::optional<Entry> lookup(const std::string& path);
    // resolveBuildTarget() over this index
    std::optional<BuildTarget> resolve(const Entry& e);

  private:
    std::unique_ptr<Statement> byPath_;
    std::unique_ptr<Statement> byPrefix_;
};

// ---- Artifact manifest (build_manifest.cpp) ---------------------------------

// One name in a directory listing
struct DirectoryName {
    std::string name;  // UTF-8 (io::toUtf8)
    bool directory = false;
};

// The content of a "<hash>.pending" marker (pending_markers.cpp)
struct PendingMarker {
    // Unix time of the last attempt; 0 when the first line is not a valid timestamp
    time_t lastAttempt = 0;
    // Dependency names still missing at that attempt (dataset-relative, see PendingBuildInfo)
    std::vector<std::string> missingDependencies;
};

// The filesystem queries of a build state read, one virtual call each, so a test can count
// them (the probe budget of DDBGetBuildArtifacts) or fake them. Every query is side-effect
// free: nothing is created, written, locked or removed.
class BuildProbe {
  public:
    virtual ~BuildProbe() = default;
    // The names directly inside dir; nothing when dir does not exist or cannot be read
    virtual std::optional<std::vector<DirectoryName>> list(const fs::path& dir) const;
    // A regular file exists at path
    virtual bool isFile(const fs::path& path) const;
    // The pending marker at path; nothing when there is none
    virtual std::optional<PendingMarker> readPendingMarker(const fs::path& path) const;
    // BuildLock::isHeld(outputFolder): never reclaims a stale lock
    virtual bool isLockHeld(const fs::path& outputFolder) const;
    // handler.isComplete(root): the rule of isBuildComplete() and getBuildStatus()
    virtual bool isComplete(const EntryHandler& handler, const fs::path& root) const;
};

// The real filesystem
const BuildProbe& filesystemBuildProbe();

// Reads the build state of entries of one index (one connection for a whole batch).
// A non-buildable entry costs no filesystem query. A buildable one costs:
//   1 listing of its build root "<build root>/<hash>" (which also tells whether a lock file
//     exists, and which artifact folders do);
//   1 read of "<hash>.pending";
//   1 lock probe, only when "<subfolder>.building" exists;
//   1 query per artifact folder that exists (a listing for file artifacts, the index file of
//     a directory artifact), a legacy artifact only while the role it stands in for is missing;
//   the completeness check of getBuildStatus(), only when the state is neither building nor
//     pending and the build root exists.
class BuildStateReader {
  public:
    explicit BuildStateReader(Database* db, const BuildProbe& probe = filesystemBuildProbe());

    // Reads path, hash and type of the entry at path; nothing when there is none
    std::optional<Entry> lookup(const std::string& path) { return targets_.lookup(path); }
    // The build state of an indexed entry (path, hash and type are read)
    EntryBuild read(const Entry& e);

  private:
    IndexBuildTargets targets_;
    fs::path buildDirectory_;
    const BuildProbe& probe_;
};

// The artifacts of handler that exist under root, in manifest order: a file artifact when
// its file exists, a directory artifact when its index file exists; a legacy artifact only
// while the role it stands in for (fallbackFor) is missing. rootNames is the listing of root (probe.list(root)).
std::vector<BuildArtifact> existingArtifacts(const EntryHandler& handler, const fs::path& root,
                                             const std::vector<DirectoryName>& rootNames,
                                             const BuildProbe& probe);

// Removes, under root, the folders of the legacy artifacts of handler whose role has an artifact
// now (a build has just published it): the old output is stale and served no more. A legacy
// artifact whose role is still missing stays, so a failed build leaves the entry as it was.
// Best effort: a folder that cannot be removed is logged as a warning and left for the next
// call; this never throws for it, so the build that called it keeps its result.
void removeSupersededArtifacts(const EntryHandler& handler,
                               const fs::path& root,
                               const BuildProbe& probe);

// ---- Build dispatch (build_dispatch.cpp) ----------------------------------

// One build attempt of an entry (or of the main file it is a sidecar of).
void buildInternal(Database* db, const Entry& e, const std::string& outputPath, bool force,
                   const BuildOptions& options = {});

// Throws RetryableBuildException(LowDiskSpace) when the filesystem of dir has less free space
// than the free-disk floor (ProcessConfig::minFreeDiskMb: DDBConfigure "minFreeDiskMb", else
// DDB_MIN_FREE_DISK_MB, else 512 MiB; 0 disables the check) before a build or a copy of build
// artifacts writes to it.
void ensureSufficientDiskSpace(const fs::path& dir);

// ---- Pending markers (pending_markers.cpp) --------------------------------

// Minimum delay between two attempts of the same pending build, in seconds since the
// timestamp on the first line of its ".pending" file (buildPending(); force bypasses it).
inline constexpr time_t PendingRetryBackoffSeconds = 60;

// The first Unix second at which buildPending() (without force) retries a pending build last
// attempted at lastAttempt: lastAttempt + PendingRetryBackoffSeconds, saturated at the largest
// time_t. The one retry rule: buildPending() skips a marker while now < pendingRetryAfter(), and
// the build manifest and getPendingBuildInfo() report it as "retryAfter".
constexpr time_t pendingRetryAfter(time_t lastAttempt) {
    return lastAttempt > std::numeric_limits<time_t>::max() - PendingRetryBackoffSeconds
               ? std::numeric_limits<time_t>::max()
               : lastAttempt + PendingRetryBackoffSeconds;
}

// Replaces "<hash>.pending" with a fresh timestamp and the given dependency
// lines, through an atomic rename of a sibling. A failed write or rename is
// logged and returns false; the previous marker is then kept.
bool replacePendingMarker(const fs::path& pendingPath, const std::string& dependencyLines);

// Replaces the marker at to with a copy of the marker at from (its timestamp and dependency
// lines), through the same atomic rename as replacePendingMarker(). Throws FSException when
// from cannot be read or to cannot be written; the previous marker at to is then kept.
void copyPendingMarker(const fs::path& from, const fs::path& to);

// Reads the marker at pendingPath without consuming it; nothing when it cannot be opened (no
// marker). A malformed timestamp reads as 0 (the build is due), as buildPending() treats it.
std::optional<PendingMarker> readPendingMarker(const fs::path& pendingPath);

// The sibling written by replacePendingMarker() before the atomic rename is
// "<hash>.pending.tmp-<rand16>" (pendingMarkerTempInfix()). A crash between write
// and rename leaves it behind; cleanupBuild removes such leftovers once they are old.
// True for "<64 hex>.pending.tmp-<non-empty suffix>".
bool isPendingMarkerTempName(const std::string& name);

// A pending marker temp file younger than this may belong to a live
// replacePendingMarker() call (its rename follows the close within
// microseconds); only older leftovers are collected.
inline constexpr std::chrono::hours PendingMarkerTempMinAge{1};

}  // namespace ddb

#endif  // BUILD_INTERNAL_H
