/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "delta_apply.h"

#include <algorithm>
#include <optional>
#include <string_view>
#include <unordered_set>

#include "cancellation.h"
#include "delta.h"
#include "entry_path.h"
#include "exceptions.h"
#include "hash.h"
#include "logger.h"
#include "mio.h"
#include "parallel.h"
#include "raster_cache.h"
#include "dbops.h"

namespace ddb {

void ensureParentFolderExists(const fs::path& folder) {
    if (folder.has_parent_path()) {
        const auto parentPath = folder.parent_path();
        io::assureFolderExists(parentPath);
    }
}

void to_json(json& j, const Conflict& conflict) {
    j = conflict.path;
}

VerifiedHashes parseVerifiedHashes(const json& verifiedHashes) {
    if (!verifiedHashes.is_object())
        throw InvalidArgsException("verifiedHashes must be a JSON object (path: SHA-256)");
    VerifiedHashes hashes;
    for (const auto& [path, hash] : verifiedHashes.items()) {
        if (!hash.is_string())
            throw InvalidArgsException("The verified hash of " + path + " must be a string");
        hashes.emplace(path, hash.get<std::string>());
    }
    return hashes;
}

bool verifyPushHashesByDefault() {
    const char* env = std::getenv("DDB_VERIFY_PUSH_HASHES");
    return env == nullptr || (env[0] != '0' && env[0] != '\0');
}

namespace {

// The file adds of `d` whose hash applyDelta checks: all but those in `verified` (XI-05). An
// entry of `verified` must name a file the delta adds and carry the delta's hash for it: the
// caller checked other bytes than the delta declares, or another file, and a hash it did not
// verify is never trusted. Both throw InvalidArgsException, before anything changes.
std::vector<const AddAction*> filesToVerify(const Delta& d, const VerifiedHashes& verified) {
    std::vector<const AddAction*> files;
    std::unordered_set<std::string_view> matched;
    for (const auto& add : d.adds) {
        if (add.isDirectory())
            continue;
        const auto it = verified.find(add.path);
        if (it == verified.end()) {
            files.push_back(&add);
            continue;
        }
        if (it->second != add.hash)
            throw InvalidArgsException("Verified hash mismatch for " + add.path + ": the delta has " + add.hash +
                                       ", the caller verified " + it->second);
        matched.insert(it->first);
    }
    if (matched.size() != verified.size())
        for (const auto& [path, hash] : verified)
            if (!matched.contains(path))
                throw InvalidArgsException("Verified hash for " + path + ", which the delta does not add as a file");
    return files;
}

// Checks that every file in `files` has, in `sourcePath`, the hash the delta declares. Runs
// before applyDelta changes anything, on the I/O threads (ioThreads: the reads dominate), with
// no transaction open.
void verifyDeltaHashes(const std::vector<const AddAction*>& files, const fs::path& sourcePath) {
    if (files.empty())
        return;

    parallel::forEachIndexIo(files.size(), [&](size_t i) {
        const auto& add = *files[i];
        const auto actual = Hash::fileSHA256(io::toUtf8(sourcePath / io::fromUtf8(add.path)));
        if (actual != add.hash)
            throw AppException("Hash mismatch for " + add.path + ": expected " + add.hash + ", got " + actual);
    });
}

// A path of a delta in its normalized spelling (entry_path.h), or an InvalidPathException when it
// names something outside the dataset or in its .ddb folder. "" stays: older versions listed the
// dataset root so, and applyDelta skips it.
std::string deltaPath(const std::string& path) {
    return optionalEntryPath(path, "path in delta");
}

// `d` with every path normalized: the delta comes from another host, and its paths are written
// under the dataset (and under the hard link folder of computeDeltaLocals)
Delta withNormalizedPaths(Delta d) {
    for (auto& add : d.adds)
        add.path = deltaPath(add.path);
    for (auto& rem : d.removes)
        rem.path = deltaPath(rem.path);
    return d;
}

// The verified hashes under the normalized spelling of their paths, as the delta has them
VerifiedHashes withNormalizedPaths(const VerifiedHashes& verified) {
    VerifiedHashes normalized;
    for (const auto& [path, hash] : verified) {
        const auto key = deltaPath(path);
        const auto [it, inserted] = normalized.emplace(key, hash);
        if (!inserted && it->second != hash)
            throw InvalidArgsException("Two verified hashes for " + key + ": paths that name the same file");
    }
    return normalized;
}

// Decides whether delta entries can be applied over the destination's version of a path, by the
// merge strategy; the entries it refuses because both sides changed them are its conflicts
class ApplyPolicy {
  public:
    ApplyPolicy(Database* destination, MergeStrategy mergeStrategy, std::vector<Conflict>& conflicts)
        : destination_(destination), mergeStrategy_(mergeStrategy), conflicts_(conflicts) {}

    struct Verdict {
        bool apply;    // the delta entry may be applied
        bool indexed;  // the destination has an entry at the path
    };

    // A delta entry can be applied over the destination's version of `path`: there is none, it
    // already matches, or it did not change since the stamp the delta was computed from
    // (`baseHash`). Otherwise both sides changed it: the merge strategy decides.
    Verdict check(const std::string& path, const std::string& hash, const std::optional<std::string>& baseHash,
                  ConflictType conflictType) {
        const auto existing = getEntry(destination_, path);
        const bool indexed = existing.has_value();
        if (!indexed || hash == existing->hash || (baseHash && *baseHash == existing->hash))
            return {true, indexed};

        switch (mergeStrategy_) {
            case MergeStrategy::DontMerge:
                conflicts_.push_back(Conflict(path, conflictType));
                return {false, indexed};
            case MergeStrategy::KeepOurs:
                return {false, indexed};
            case MergeStrategy::KeepTheirs:
            default:
                return {true, indexed};
        }
    }

  private:
    Database* destination_;
    MergeStrategy mergeStrategy_;
    std::vector<Conflict>& conflicts_;
};

}  // namespace

namespace {

// The removes of the delta that apply: the files are removed from the index and the disk, children
// before their folder. We don't check whether the file has been modified on the FS, perhaps we should?
void applyRemoves(const Delta& d, Database* destination, ApplyPolicy& policy, std::ostream& out) {
    const fs::path destPath = destination->rootDirectory();
    std::vector<std::string> indexedRemovals;
    std::vector<const RemoveAction*> fileRemovals;
    for (const auto& rem : d.removes) {
        // The dataset root (older versions indexed it as "") is never removed
        if (rem.path.empty())
            continue;

        // rem.hash is the hash in the destination stamp
        const auto verdict = policy.check(rem.path, rem.hash, rem.hash, ConflictType::RemoteDeleteLocalModified);
        if (!verdict.apply)
            continue;

        const auto dest = destPath / io::fromUtf8(rem.path);
        if (fs::exists(dest)) {
            if (verdict.indexed)
                indexedRemovals.push_back(io::toUtf8(dest));
            fileRemovals.push_back(&rem);
        }
    }

    // Children before their folder even when the delta is not sorted: removing a folder
    // removes its content from the index
    std::sort(indexedRemovals.rbegin(), indexedRemovals.rend());
    std::sort(fileRemovals.begin(), fileRemovals.end(),
              [](const RemoveAction* l, const RemoveAction* r) { return l->path > r->path; });

    removeFromIndex(destination, indexedRemovals);
    for (const auto* rem : fileRemovals) {
        // A raster kept open by recent tile requests would block this on Windows
        releaseCachedFiles(destPath / io::fromUtf8(rem->path));
        io::assureIsRemoved(destPath / io::fromUtf8(rem->path));
        out << "D\t" << rem->path << std::endl;
    }
}

void indexAddedFiles(Database* destination, const std::vector<KnownFile>& files, std::ostream& out) {
    addToIndexWithHashes(destination, files, [&out](const Entry& e, bool updated) {
        out << (updated ? "U" : "A") << "\t" << e.path << std::endl;
        return true;
    });
}

// The adds of the delta that apply (sorted by path: folders before their content). Files are moved,
// not copied: the source folder is a temporary folder on the same volume.
void applyAdds(const Delta& d, Database* destination, const fs::path& sourcePath, ApplyPolicy& policy,
               std::ostream& out) {
    const fs::path destPath = destination->rootDirectory();
    std::vector<KnownFile> addedFiles;
    try {
        for (const auto& add : d.adds) {
            if (add.path.empty())
                continue;

            if (!policy.check(add.path, add.hash, add.baseHash, ConflictType::BothModified).apply)
                continue;

            const auto dest = destPath / io::fromUtf8(add.path);
            if (add.isDirectory()) {
                io::createDirectories(dest);
            } else {
                releaseCachedFiles(dest);
                io::moveOrCopy(sourcePath / io::fromUtf8(add.path), dest);
            }

            addedFiles.push_back({io::toUtf8(dest), add.hash});
        }
    } catch (const std::exception& e) {
        // The files already moved are gone from the source folder: index them, so the
        // index matches the dataset and a new delta does not ask for them again
        LOGW << "Delta apply failed after " << addedFiles.size() << " adds: " << e.what();
        try {
            indexAddedFiles(destination, addedFiles, out);
        } catch (const std::exception& ie) {
            LOGW << "Cannot index the adds applied before the failure: " << ie.what();
        }
        throw;
    }

    indexAddedFiles(destination, addedFiles, out);
}

// The metadata the delta adds is restored from the dump of the source; the metadata it removes is removed
void applyMeta(const Delta& d, Database* destination, const json& sourceMetaDump) {
    if (d.metaAdds.size() > 0) {
        json metaRestore = json::array();

        std::unordered_map<std::string, bool> metaIds;
        for (auto& id : d.metaAdds)
            metaIds[id] = true;

        for (auto& meta : sourceMetaDump) {
            if (!meta.contains("id"))
                throw InvalidArgsException("Invalid meta element: " + meta.dump());
            if (metaIds.find(meta["id"]) != metaIds.end())
                metaRestore.push_back(meta);
        }

        destination->getMetaManager()->restore(metaRestore);
    }

    if (d.metaRemoves.size() > 0) {
        destination->getMetaManager()->bulkRemove(d.metaRemoves);
    }
}

}  // namespace

std::vector<Conflict> applyDelta(const Delta& rawDelta,
                                 Database* destination,
                                 const DeltaApplication& application,
                                 std::ostream& out) {
    cancel::throwIfCanceled();
    std::vector<Conflict> conflicts;
    ApplyPolicy policy(destination, application.mergeStrategy, conflicts);
    // Before anything changes: a path outside the dataset refuses the whole delta
    const Delta d = withNormalizedPaths(rawDelta);
    const auto unverifiedFiles = filesToVerify(d, withNormalizedPaths(application.verifiedHashes));

    // Reading and hashing the files is the long part and changes nothing: it stops when the call
    // is canceled. Then the delta is applied whole: the files are moved, the index and the
    // metadata change, and a cancel (or a shutdown) after this point is not seen until the
    // delta is applied, so a canceled DDBApplyDelta leaves the dataset as it was, never half done.
    if (application.verifyHashes)
        verifyDeltaHashes(unverifiedFiles, application.sourcePath);
    cancel::throwIfCanceled();
    const cancel::UninterruptibleScope applyWhole;
    LOGD << "Applying a delta: " << d.adds.size() << " adds, " << d.removes.size() << " removes, "
         << d.metaAdds.size() << " metadata adds, " << d.metaRemoves.size() << " metadata removes";

    // File operations
    if (d.adds.size() > 0 || d.removes.size() > 0) {
        applyRemoves(d, destination, policy, out);
        applyAdds(d, destination, application.sourcePath, policy, out);

        if (conflicts.size() == 0) {
            auto mPathList = d.modifiedPathList();
            if (mPathList.size() > 0)
                syncLocalMTimes(destination, mPathList);
        }
    }

    // Early exit in case of conflicts
    if (conflicts.size() > 0)
        return conflicts;

    applyMeta(d, destination, application.sourceMetaDump);
    return conflicts;
}

std::unordered_map<std::string, bool> computeDeltaLocals(Delta d,
                                                         Database* db,
                                                         const std::string& hlDestFolder) {
    d = withNormalizedPaths(std::move(d));

    // Do we have files locally? If so we don't need to
    // download them, we just create hard links (or copies) to it after
    // validating that they are indeed the same
    // This function creates hard links only if hlDestFolder is set
    // Otherwise it just returns the map of valid local hashes
    const auto q = db->query("SELECT path,mtime FROM entries WHERE hash = ?");
    std::unordered_map<std::string, bool> localMap;

    std::unordered_map<std::string, bool> addsMap;
    for (auto& add : d.adds) {
        if (add.hash.empty())
            continue;
        addsMap[add.path] = true;
    }

    for (auto& add : d.adds) {
        if (add.hash.empty())
            continue;

        // Checking a local file hashes it when it changed: a checkpoint of the call (DQ-15)
        cancel::throwIfCanceled();
        q->bind(1, add.hash);
        if (q->fetch()) {
            auto ePath = q->getText(0);
            // Check the filesystem to make sure this hasn't been modified
            io::Path p(db->rootDirectory() / io::fromUtf8(ePath));
            const double eMtime = q->getDouble(1);
            bool valid = false;
            if (io::sameModifiedTime(eMtime, p.getModifiedTimePrecise())) {
                valid = true;
            } else {
                // Actually compute hash
                valid = Hash::fileSHA256(io::toUtf8(p.get())) == add.hash;
            }

            if (valid) {
                if (!hlDestFolder.empty()) {
                    const auto destPath = io::fromUtf8(hlDestFolder) / io::fromUtf8(add.path);
                    io::createDirectories(destPath.parent_path());

                    // We can leverage hard links ONLY
                    // if the path we are linking is not itself
                    // part of an add operation (otherwise it could be
                    // overwritten)
                    if (addsMap.find(ePath) == addsMap.end())
                        io::hardlinkSafe(p.get(), destPath);
                    else
                        io::copy(p.get(), destPath);
                }
                localMap[add.hash] = true;
            }
        }
        q->reset();
    }

    return localMap;
}

}  // namespace ddb
