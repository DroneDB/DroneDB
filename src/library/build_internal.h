/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef BUILD_INTERNAL_H
#define BUILD_INTERNAL_H

// Private to the build implementation (build.cpp, build_dispatch.cpp,
// pending_markers.cpp, build_cleanup.cpp): not installed, not part of the API.

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "dbops.h"
#include "entry_handler.h"
#include "fs.h"

namespace ddb {

// Reads an indexed entry by path; false when there is none.
using EntryLookup = std::function<bool(const std::string&, Entry&)>;
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

// ---- Build dispatch (build_dispatch.cpp) ----------------------------------

// One build attempt of an entry (or of the main file it is a sidecar of).
void buildInternal(Database* db, const Entry& e, const std::string& outputPath, bool force);

// ---- Pending markers (pending_markers.cpp) --------------------------------

// Replaces "<hash>.pending" with a fresh timestamp and the given dependency
// lines, through an atomic rename of a sibling. A failed write or rename is
// logged and returns false; the previous marker is then kept.
bool replacePendingMarker(const fs::path& pendingPath, const std::string& dependencyLines);

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
