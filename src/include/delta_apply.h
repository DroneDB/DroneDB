/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DELTA_APPLY_H
#define DELTA_APPLY_H

// Applying a delta to a dataset (DQ-11: out of registry.h, where it lived with the network
// client although it touches no network: pull and DDBApplyDelta both use it).

#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "database.h"
#include "ddb_export.h"
#include "fs.h"
#include "json.h"

namespace ddb {

struct Delta;

enum MergeStrategy {
    DontMerge = 0,
    KeepTheirs = 1,
    KeepOurs = 2
};

enum ConflictType {
    RemoteDeleteLocalModified = 0,
    BothModified = 1
};

struct Conflict {
    std::string path;
    ConflictType type;

    Conflict(const std::string &conflictPath, ConflictType conflictType) : path(conflictPath), type(conflictType) {}

    std::string description() {
        switch (type) {
            case ConflictType::RemoteDeleteLocalModified:
                return "deleted on remote but modified locally";
            case ConflictType::BothModified:
                return "both modified";
            default:
                return "should not have happend";
        }
    }
};

// The files of a delta whose content the caller already checked (XI-05): delta path ->
// the SHA-256 the caller computed (e.g. while it received the file)
using VerifiedHashes = std::unordered_map<std::string, std::string>;

/// How a delta is applied to a dataset
struct DeltaApplication {
    /// Where the added files are (a temporary folder on the volume of the dataset): they are
    /// moved, not copied
    fs::path sourcePath;
    MergeStrategy mergeStrategy = DontMerge;
    /// The metadata of the source dataset, from which the metadata the delta adds is restored
    json sourceMetaDump = json::array();
    /// Every added file is checked against the hash in the delta before anything changes, and a
    /// mismatch throws; the index stores the delta's hashes
    bool verifyHashes = true;
    /// A file here is not read again: its entry must name a file the delta adds, with the delta's
    /// hash, or applyDelta throws InvalidArgsException before anything changes (also without
    /// verifyHashes)
    VerifiedHashes verifiedHashes;
};

/**
 * Applies a delta to `destination`, as `application` says. What it does to the files is
 * written to `out` (one "A", "U" or "D" line per file). Returns the conflicts.
 */
DDB_DLL std::vector<Conflict> applyDelta(const Delta &d, Database *destination,
                                         const DeltaApplication &application, std::ostream &out);
DDB_DLL void ensureParentFolderExists(const fs::path &folder);
DDB_DLL std::unordered_map<std::string, bool> computeDeltaLocals(Delta d, Database *destination,
                                                                const std::string &hlDestFolder);

} // namespace ddb

#endif // DELTA_APPLY_H
