/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef BUILD_MANIFEST_H
#define BUILD_MANIFEST_H

#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dbops.h"
#include "entry_handler.h"
#include "fs.h"
#include "json_fwd.h"

/**
 * The build manifest: what a host needs to serve the builds of a dataset
 * without knowing their layout. libddb answers, from the EntryHandler registry, which entry
 * types exist and what they build (entryTypesJson), the state and the existing artifacts of
 * entries (BuildStateReader, getBuildArtifactsJson, listEntriesWithBuildJson), the file behind
 * a build URL (resolveBuildFile) and the copy of a build to another dataset
 * (copyBuildArtifacts). The JSON shapes are the C API's (ddb.h) and are pinned by the contract
 * fixtures (tests/contract).
 */
namespace ddb {

// The state of the build of an entry, in this order of precedence:
//   Building  a live BuildLock holds its output folder (BuildLock::isHeld: a stale lock file
//             is not a build, and is left alone)
//   Pending   "<hash>.pending" exists: a build deferred until its missing dependencies arrive
//   Complete  the artifacts are complete (isBuildComplete)
//   None      none of the above (not built yet, failed, or an incomplete output)
enum class BuildState { None, Pending, Building, Complete };

// "none", "pending", "building", "complete"
std::string_view buildStateName(BuildState state);

// The build of one entry: the "build" object of DDBGetBuildArtifacts and DDBListEntriesEx
struct EntryBuild {
    // The entry is built, directly or as a sidecar of its main file (isBuildable)
    bool buildable = false;
    // The entry whose artifacts these are: the entry itself, or the main file a sidecar
    // belongs to ("" when not buildable)
    std::string source;
    // The hash of source, the name of its build root ("" when not buildable)
    std::string hash;
    // The absolute build root "<dataset>/.ddb/build/<hash>" (empty when not buildable)
    fs::path root;
    BuildState state = BuildState::None;
    // From "<hash>.pending", when it exists
    std::vector<std::string> missingDependencies;
    std::optional<time_t> lastAttempt;
    // The first Unix second at which buildPending() (without force) retries the pending build:
    // lastAttempt + the retry backoff (60 s); set exactly when lastAttempt is
    std::optional<time_t> retryAfter;
    // The artifacts that exist, in the type's manifest order (EntryHandler::artifacts()); the
    // primary output first
    std::vector<BuildArtifact> artifacts;
};

// {"role", "path", "kind": "file"|"dir", "mediaType", "downloadName"?, "index"?, "legacy"?, "fallbackFor"?}
void to_json(json& j, const BuildArtifact& artifact);
// {"buildable", "source", "hash", "root", "state", "missingDependencies", "lastAttempt",
// "retryAfter", "artifacts"}; source, hash, root, lastAttempt and retryAfter are null when they
// do not apply
void to_json(json& j, const EntryBuild& build);

// DDBGetEntryTypes: one object per registered entry type, by id
json entryTypesJson();

// DDBGetBuildArtifacts: one object per path, in input order, from one index connection:
// {"path", "exists": false} or {"path", "exists": true, "hash", "type", "build"}
json getBuildArtifactsJson(Database* db, const std::vector<std::string>& paths);

// DDBListEntries and DDBListEntriesEx: listEntries(db, query) as a JSON array of
// to_json(Entry) (the "json" format of writeEntries), each entry with its "build" object when
// withBuild. Serialized entry by entry: the listing is never held as one JSON document.
std::string listEntriesJson(Database* db, const EntryQuery& query, bool withBuild);

// DDBResolveBuildFile: a file of a build and the type it is served as
struct ResolvedBuildFile {
    fs::path path;           // absolute
    std::string mediaType;   // of the artifact when path is the file a client opens (a file
                             // artifact, the index of a directory); empty for any other file
                             // (the tiles of a directory artifact)

    // {"path": "<absolute path, UTF-8>", "mediaType": "<type>"}; "mediaType" is absent when empty
    json toJson() const;
};

// The file at relativePath ('/'-separated) in the build root of hash, when relativePath lies
// inside one of the artifacts of an entry with that hash (a file artifact itself, or a file
// below a directory artifact).
// Throws InvalidArgsException when hash is not a SHA-256 hex string or relativePath breaks the
// dataset path rules (normalizeEntryPath: absolute, "..", empty, invalid UTF-8, ...);
// EntryNotFoundException when no entry has that hash, the path is not inside one of its
// artifacts, the file does not exist, or it resolves (through a link) outside the build root.
ResolvedBuildFile resolveBuildFile(Database* db, const std::string& hash, const std::string& relativePath);

// DDBCopyBuildArtifacts: copies the artifact folders of hash (those of the types of the
// entries of source with that hash, legacy folders included) and its pending marker from the
// build root of source to the one of destination, replacing what the destination had in
// those folders. Returns {"hash", "artifacts": [...the existing artifacts of the destination
// afterwards...], "pending": <the destination has a pending marker>}.
// Throws InvalidArgsException for an invalid hash or when both are the same dataset,
// EntryNotFoundException when no entry of source has that hash, BuildInProgressException
// when a build of hash runs in either dataset (nothing is copied), RetryableBuildException
// when the destination's disk is (nearly) full.
json copyBuildArtifacts(Database* source, Database* destination, const std::string& hash);

}  // namespace ddb

#endif  // BUILD_MANIFEST_H
