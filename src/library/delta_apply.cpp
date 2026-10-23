/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "delta_apply.h"

#include <algorithm>
#include <optional>
#include <string_view>
#include <unordered_set>

#include "delta.h"
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

// A delta path must name something inside the dataset
void checkDeltaPaths(const Delta& d) {
    const auto check = [](const std::string& path) {
        const fs::path p = io::fromUtf8(path);
        const bool escapes = std::any_of(p.begin(), p.end(), [](const fs::path& part) { return part == ".."; });
        if (p.is_absolute() || p.has_root_name() || p.has_root_directory() || escapes)
            throw InvalidArgsException("Invalid path in delta: " + path);
    };
    for (const auto& add : d.adds)
        check(add.path);
    for (const auto& rem : d.removes)
        check(rem.path);
}

// Whether a delta entry can be applied over the destination's version of `path`: there is
// none, it already matches, or it did not change since the stamp the delta was computed
// from (`baseHash`). Otherwise both sides changed it: the merge strategy decides.
bool canApply(Database* destination,
              const std::string& path,
              const std::string& hash,
              const std::optional<std::string>& baseHash,
              const MergeStrategy mergeStrategy,
              const ConflictType conflictType,
              std::vector<Conflict>& conflicts,
              bool& indexed) {
    Entry e;
    indexed = getEntry(destination, path, e);
    if (!indexed || hash == e.hash || (baseHash && *baseHash == e.hash))
        return true;

    switch (mergeStrategy) {
        case MergeStrategy::DontMerge:
            conflicts.push_back(Conflict(path, conflictType));
            return false;
        case MergeStrategy::KeepOurs:
            return false;
        case MergeStrategy::KeepTheirs:
        default:
            return true;
    }
}

}  // namespace

std::vector<Conflict> applyDelta(const Delta& d,
                                 const fs::path& sourcePath,
                                 Database* destination,
                                 const MergeStrategy mergeStrategy,
                                 const json& sourceMetaDump,
                                 std::ostream& out,
                                 bool verifyHashes,
                                 const VerifiedHashes& verifiedHashes) {
    std::vector<Conflict> conflicts;
    const auto unverifiedFiles = filesToVerify(d, verifiedHashes);

    // File operations
    if (d.adds.size() > 0 || d.removes.size() > 0) {
        const fs::path destPath = destination->rootDirectory();

        checkDeltaPaths(d);
        if (verifyHashes)
            verifyDeltaHashes(unverifiedFiles, sourcePath);

        // Removes. We don't check whether the file has been modified on the FS, perhaps we should?
        std::vector<std::string> indexedRemovals;
        std::vector<const RemoveAction*> fileRemovals;
        for (const auto& rem : d.removes) {
            // The dataset root (older versions indexed it as "") is never removed
            if (rem.path.empty())
                continue;

            // rem.hash is the hash in the destination stamp
            bool indexed;
            if (!canApply(destination, rem.path, rem.hash, rem.hash, mergeStrategy,
                          ConflictType::RemoteDeleteLocalModified, conflicts, indexed))
                continue;

            const auto dest = destPath / io::fromUtf8(rem.path);
            if (fs::exists(dest)) {
                if (indexed)
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

        const auto indexAdded = [&](const std::vector<KnownFile>& files) {
            addToIndexWithHashes(destination, files, [&out](const Entry& e, bool updated) {
                out << (updated ? "U" : "A") << "\t" << e.path << std::endl;
                return true;
            });
        };

        // Adds (sorted by path: folders before their content). Files are moved, not
        // copied: the source folder is a temporary folder on the same volume.
        std::vector<KnownFile> addedFiles;
        try {
            for (const auto& add : d.adds) {
                if (add.path.empty())
                    continue;

                bool indexed;
                if (!canApply(destination, add.path, add.hash, add.baseHash, mergeStrategy,
                              ConflictType::BothModified, conflicts, indexed))
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
                indexAdded(addedFiles);
            } catch (const std::exception& ie) {
                LOGW << "Cannot index the adds applied before the failure: " << ie.what();
            }
            throw;
        }

        indexAdded(addedFiles);

        if (conflicts.size() == 0) {
            auto mPathList = d.modifiedPathList();
            if (mPathList.size() > 0)
                syncLocalMTimes(destination, mPathList);
        }
    }

    // Early exit in case of conflicts
    if (conflicts.size() > 0)
        return conflicts;

    // Meta operations
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

    return conflicts;
}

std::unordered_map<std::string, bool> computeDeltaLocals(Delta d,
                                                         Database* db,
                                                         const std::string& hlDestFolder) {
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
