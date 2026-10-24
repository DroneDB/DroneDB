/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Build clean-up: stale index entries, orphan build folders and markers, and the
// scratch folders a crashed build left behind (cleanupBuild).

#include "build.h"
#include "randomutil.h"

#include <optional>
#include <unordered_set>

#include "3d.h"
#include "build_internal.h"
#include "build_layout.h"
#include "buildlock.h"
#include "dbops.h"
#include "exceptions.h"
#include "mio.h"

namespace ddb {

// Removes a leftover build folder and records it in the cleanup result.
static void removeOrphanFolder(const fs::path& p, CleanupResult& result) {
    std::error_code rmErr;
    fs::remove_all(p, rmErr);
    if (rmErr) {
        LOGW << "Failed to remove orphan temp folder '" << io::toUtf8(p)
             << "': " << rmErr.message();
    } else {
        result.removedBuilds.push_back(io::toUtf8(p));
        LOGD << "Removed orphan build temp folder: " << io::toUtf8(p);
    }
}

// Prefix of 3D Tiles leftovers already moved aside by a cleanup
// (tiles3DTrashPrefix(), "3dtiles-temp-trash-"). It still matches "3dtiles-temp-*",
// so a trash folder whose deletion failed is picked up by the next cleanup. No
// build ever uses such a folder (buildModel3DTiles names its temp folders
// "3dtiles-temp-<rand16>", without dashes in the suffix), so it is deleted
// without holding the "3dtiles" lock.

// The 3D Tiles siblings "3dtiles-temp-<rand>" and "3dtiles-old" belong to
// buildModel3DTiles, which runs under the "3dtiles" lock inside a Model build
// that already holds the primary "nxs" lock (buildInternal); the standalone
// "ddb tiles3d" command holds only the "3dtiles" lock.
//
// Lock order here is the same as in a build: first the primary "nxs" lock, then
// the "3dtiles" lock, both non-blocking. Holding "nxs" means no Model build can
// be in flight, so a build can never meet this cleanup on the "3dtiles" lock
// (a refusal there would be swallowed as a best-effort tiles failure and the
// tiles would never be produced, because completeness is keyed on nxs/). A
// Model build that arrives while both locks are held is refused on its primary
// lock instead, which every caller already treats as benign contention
// (buildPending leaves the marker untouched, the Registry skips the job and
// its completeness sweep re-enqueues the incomplete entry). If either lock is
// busy the siblings are left alone until the next cleanup.
//
// Under the locks the siblings are only renamed to unique
// "3dtiles-temp-trash-<rand16>" names; both locks are released before the
// (possibly long) deletion. "3dtiles-old" may be the only usable tileset when
// the final directory is absent or incomplete, so it is moved aside only once
// the final has its expected nonempty regular sentinel. trashFolders are
// leftovers of an earlier cleanup: they are deleted whatever the lock state.
static void removeOrphanTilesFolders(const fs::path& hashDir,
                                     const std::vector<fs::path>& tempFolders,
                                     const fs::path& oldFolder,
                                     const std::vector<fs::path>& trashFolders,
                                     CleanupResult& result) {
    const fs::path primaryDir = hashDir / NexusBuildSubfolder;
    const fs::path tilesDir = hashDir / Tiles3DBuildSubfolder;

    // (trash path, path to report once deleted)
    std::vector<std::pair<fs::path, fs::path>> toDelete;
    for (const auto& t : trashFolders)
        toDelete.emplace_back(t, t);

    if (!tempFolders.empty() || !oldFolder.empty()) {
        // Declared in acquisition order: destruction releases "3dtiles" first,
        // then "nxs" (reverse order).
        std::optional<BuildLock> primaryLock;
        std::optional<BuildLock> tilesLock;
        try {
            primaryLock.emplace(io::toUtf8(primaryDir), false);
        } catch (const BuildInProgressException&) {
            LOGD << "Skipping 3D Tiles temp folders: a Model build is in progress in "
                 << io::toUtf8(hashDir);
        } catch (const AppException& e) {
            LOGW << "Cannot lock " << io::toUtf8(primaryDir) << " (" << e.what()
                 << "); skipping 3D Tiles temp folders";
        }
        if (primaryLock) {
            try {
                tilesLock.emplace(io::toUtf8(tilesDir), false);
            } catch (const BuildInProgressException&) {
                LOGD << "Skipping 3D Tiles temp folders with active tiles build lock in: "
                     << io::toUtf8(hashDir);
            } catch (const AppException& e) {
                LOGW << "Cannot lock " << io::toUtf8(tilesDir) << " (" << e.what()
                     << "); skipping 3D Tiles temp folders";
            }
        }

        if (tilesLock) {
            const auto moveAside = [&](const fs::path& p) {
                const fs::path trash = hashDir / (tiles3DTrashPrefix() + randomString(16));
                std::error_code ec;
                fs::rename(p, trash, ec);
                if (ec) {
                    LOGW << "Cannot move orphan folder '" << io::toUtf8(p) << "' aside for deletion: "
                         << ec.message();
                    return;
                }
                toDelete.emplace_back(trash, p);
            };

            for (const auto& t : tempFolders)
                moveAside(t);

            if (!oldFolder.empty()) {
                if (isModel3DTilesComplete(tilesDir))
                    moveAside(oldFolder);
                else
                    LOGD << "Keeping " << io::toUtf8(oldFolder) << ": " << io::toUtf8(tilesDir)
                         << " has no complete tiles sentinel; a build can restore it";
            }
        }
        // Both locks are released here ("3dtiles" then "nxs"), before any deletion.
    }

    for (const auto& [trash, reported] : toDelete) {
        std::error_code rmErr;
        fs::remove_all(trash, rmErr);
        if (rmErr) {
            LOGW << "Failed to remove orphan temp folder '" << io::toUtf8(trash)
                 << "' (the next cleanup retries): " << rmErr.message();
        } else {
            result.removedBuilds.push_back(io::toUtf8(reported));
            LOGD << "Removed orphan build temp folder: " << io::toUtf8(reported);
        }
    }
}

// Remove orphaned "<subfolder>-temp-<rand>" scratch folders inside a build hash
// directory. buildInternal() creates such a folder per build and renames it to
// the final output (or deletes it) when done; a process crash during the build
// leaves it behind together with any GDAL/COG/Nexus scratch it contains. A
// folder is removed only when there is no live build lock for its subfolder, so
// an in-progress build is never disturbed. "3dtiles-temp-*" (including the
// "3dtiles-temp-trash-*" leftovers of an earlier cleanup) and "3dtiles-old" are
// handled by removeOrphanTilesFolders.
static void removeOrphanTempFolders(const fs::path& hashDir, CleanupResult& result) {
    std::error_code ec;
    fs::directory_iterator it(hashDir, ec);
    if (ec)
        return;

    std::vector<fs::path> tilesTempFolders;
    std::vector<fs::path> tilesTrashFolders;
    fs::path tilesOldFolder;
    const std::string tilesOldName = tiles3DBackupFolderName();
    const std::string tilesTrashPrefix = tiles3DTrashPrefix();
    const std::string_view tempInfix = TempFolderInfix;

    const fs::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec)
            return;

        std::error_code typeErr;
        if (!it->is_directory(typeErr) || typeErr)
            continue;

        const std::string name = io::toUtf8(it->path().filename());
        if (name == tilesOldName) {
            tilesOldFolder = it->path();
            continue;
        }

        const auto pos = name.find(tempInfix);
        // Require a non-empty subfolder prefix and a non-empty random suffix so
        // unrelated directory names are never matched.
        if (pos == std::string::npos || pos == 0 || name.size() <= pos + tempInfix.size())
            continue;

        const std::string subfolder = name.substr(0, pos);
        if (subfolder == Tiles3DBuildSubfolder) {
            if (name.starts_with(tilesTrashPrefix))
                tilesTrashFolders.push_back(it->path());
            else
                tilesTempFolders.push_back(it->path());
            continue;
        }

        // A stale lock file is removed with the folder it guarded (Unix always did so).
        const std::string outputFolder = io::toUtf8(hashDir / subfolder);
        if (fs::exists(io::fromUtf8(buildLockPath(outputFolder))) && !BuildLock::removeIfStale(outputFolder)) {
            LOGD << "Skipping temp folder with active build lock: " << io::toUtf8(it->path());
            continue;
        }

        removeOrphanFolder(it->path(), result);
    }

    if (!tilesTempFolders.empty() || !tilesOldFolder.empty() || !tilesTrashFolders.empty())
        removeOrphanTilesFolders(hashDir, tilesTempFolders, tilesOldFolder, tilesTrashFolders, result);
}

namespace {

// Phase 1: removes the DB entries whose underlying file is gone
void removeStaleEntries(Database* db, CleanupResult& result) {
    const fs::path rootDir = db->rootDirectory();
    std::vector<std::string> missingPaths;

    {
        auto qEntries = db->query(
            "SELECT path FROM entries WHERE type <> ? ORDER BY path");
        qEntries->bind(1, entryTypeValue(EntryType::Directory));
        while (qEntries->fetch()) {
            const std::string entryPath = qEntries->getText(0);
            std::error_code existsErr;
            const bool present = fs::exists(rootDir / io::fromUtf8(entryPath), existsErr);
            if (existsErr) {
                LOGW << "Cannot stat '" << entryPath
                     << "' (skipping): " << existsErr.message();
                continue;
            }
            if (!present)
                missingPaths.push_back(entryPath);
        }
    }

    if (missingPaths.empty())
        return;

    LOGD << "cleanupBuild: removing " << missingPaths.size()
         << " stale DB entry/entries";
    // Remove entries one at a time so that a failure on a single path
    // does not lose the record of paths that were actually removed.
    // removeFromIndex expects absolute paths (it computes the relative
    // form internally via io::Path::relativeTo).
    for (const auto& rel : missingPaths) {
        const std::string absPath = io::toUtf8(rootDir / io::fromUtf8(rel));
        try {
            removeFromIndex(db, std::vector<std::string>{absPath});
            result.removedEntries.push_back(rel);
        } catch (const AppException& e) {
            LOGW << "Failed to remove stale DB entry '" << rel
                 << "': " << e.what();
        }
    }
}

// Whether a build of the orphan directory may be running: a held lock, or a directory that cannot
// be scanned for locks. Lock files live one level deep: <hash>/<subfolder>.building. Uses a
// non-recursive scan and treats any iteration error as "potentially active" to avoid wrongly
// deleting a locked dir.
bool mayHaveActiveBuild(const fs::path& p) {
    std::error_code subErr;
    fs::directory_iterator subIt(p, subErr);
    if (subErr) {
        LOGW << "Cannot scan orphan '" << io::toUtf8(p)
             << "' for active locks (" << subErr.message()
             << "); skipping removal as a precaution";
        return true;
    }

    const fs::directory_iterator subEnd;
    for (; subIt != subEnd; subIt.increment(subErr)) {
        if (subErr) {
            LOGW << "Iteration error inside '" << io::toUtf8(p)
                 << "': " << subErr.message()
                 << "; skipping removal as a precaution";
            return true;
        }
        // "<subfolder>.building" is the lock of "<subfolder>"
        if (subIt->path().extension() == BuildLockSuffix &&
            BuildLock::isHeld(io::toUtf8(p / subIt->path().stem()))) {
            LOGD << "Skipping orphan with active build lock: " << io::toUtf8(p);
            return true;
        }
    }
    return false;
}

// A directory of the build folder: a referenced hash keeps its output (and loses its stale temp
// folders), an orphan hash is removed unless a build may be running in it
void cleanupBuildDirectory(const fs::path& p, const std::string& name,
                           const std::unordered_set<std::string>& validHashes, CleanupResult& result) {
    // Directory name is expected to be a content hash. Skip anything
    // that doesn't look like one to avoid clobbering unrelated dirs.
    if (!isHashLike(name)) {
        LOGD << "Skipping non-hash directory: " << io::toUtf8(p);
        return;
    }
    if (validHashes.find(name) != validHashes.end()) {
        // Still referenced: keep the build output, but reclaim any
        // orphaned "<subfolder>-temp-<rand>" scratch folders left behind
        // by a build that crashed before its RAII cleanup ran (a SIGSEGV
        // bypasses the cleanup in buildInternal). Folders whose build is
        // still active (live .building lock) are skipped.
        removeOrphanTempFolders(p, result);
        return;  // still referenced
    }

    if (mayHaveActiveBuild(p))
        return;

    std::error_code rmErr;
    fs::remove_all(p, rmErr);
    if (rmErr) {
        LOGW << "Failed to remove orphan build directory '" << io::toUtf8(p)
             << "': " << rmErr.message();
    } else {
        result.removedBuilds.push_back(io::toUtf8(p));
        LOGD << "Removed orphan build directory: " << io::toUtf8(p);
    }
}

// <hash>.pending: removed when no entry has that hash
void cleanupPendingFile(const fs::path& p, const std::unordered_set<std::string>& validHashes,
                        CleanupResult& result) {
    // .pending file naming convention: <hash>.pending. Validate the
    // stem before deleting so we never touch unrelated *.pending files.
    const std::string hash = io::toUtf8(p.stem());
    if (!isHashLike(hash)) {
        LOGD << "Skipping non-hash pending file: " << io::toUtf8(p);
        return;
    }
    if (validHashes.find(hash) != validHashes.end())
        return;

    std::error_code rmErr;
    fs::remove(p, rmErr);
    if (rmErr) {
        LOGW << "Failed to remove orphan pending file '" << io::toUtf8(p)
             << "': " << rmErr.message();
    } else {
        result.removedBuilds.push_back(io::toUtf8(p));
        LOGD << "Removed orphan pending file: " << io::toUtf8(p);
    }
}

// Leftover of replacePendingMarker() interrupted between write and rename. It is never valid
// state, whatever its hash, but a fresh one may belong to a live writer: collect it only once it
// is old.
void cleanupPendingMarkerTemp(const fs::path& p, CleanupResult& result) {
    std::error_code timeErr;
    const auto writeTime = fs::last_write_time(p, timeErr);
    if (timeErr) {
        LOGW << "Cannot read the age of pending marker temp file '" << io::toUtf8(p)
             << "' (" << timeErr.message() << "); keeping it";
        return;
    }
    if (fs::file_time_type::clock::now() - writeTime < PendingMarkerTempMinAge) {
        LOGD << "Keeping recent pending marker temp file: " << io::toUtf8(p);
        return;
    }

    std::error_code rmErr;
    fs::remove(p, rmErr);
    if (rmErr) {
        LOGW << "Failed to remove pending marker temp file '" << io::toUtf8(p)
             << "': " << rmErr.message();
    } else {
        result.removedBuilds.push_back(io::toUtf8(p));
        LOGD << "Removed stale pending marker temp file: " << io::toUtf8(p);
    }
}

// Phase 2: removes the orphan build artifacts of the build folder
void removeOrphanArtifacts(const fs::path& buildDir, const std::unordered_set<std::string>& validHashes,
                           CleanupResult& result) {
    // Artifact names are validated with isHashLike() before deleting so we never
    // wipe unrelated files/directories that may live in a user-specified output path.

    std::error_code iterErr;
    fs::directory_iterator it(buildDir, iterErr);
    if (iterErr) {
        LOGW << "Cannot open build directory '" << io::toUtf8(buildDir)
             << "': " << iterErr.message();
        return;
    }

    const fs::directory_iterator end;
    for (; it != end; it.increment(iterErr)) {
        if (iterErr) {
            LOGW << "Error iterating build directory '" << io::toUtf8(buildDir)
                 << "': " << iterErr.message();
            break;
        }

        const auto& entry = *it;
        const auto& p = entry.path();
        const std::string name = io::toUtf8(p.filename());

        std::error_code typeErr;
        if (entry.is_directory(typeErr)) {
            cleanupBuildDirectory(p, name, validHashes, result);
        } else if (entry.is_regular_file(typeErr) && p.extension() == PendingMarkerExtension) {
            cleanupPendingFile(p, validHashes, result);
        } else if (entry.is_regular_file(typeErr) && isPendingMarkerTempName(name)) {
            cleanupPendingMarkerTemp(p, result);
        }
    }
}

}  // namespace

CleanupResult cleanupBuild(Database* db, const std::string& outputPath) {
    CleanupResult result;

    // -------- Phase 1: remove DB entries whose underlying file is gone --------
    removeStaleEntries(db, result);

    // -------- Phase 2: remove orphan build artifacts --------
    const fs::path buildDir = outputPath.empty() ? db->buildDirectory() : io::fromUtf8(outputPath);
    std::error_code dirExistsErr;
    if (!fs::exists(buildDir, dirExistsErr)) {
        if (dirExistsErr)
            LOGW << "Cannot check build directory '" << io::toUtf8(buildDir)
                 << "': " << dirExistsErr.message();
        else
            LOGD << "Build directory does not exist: " << io::toUtf8(buildDir);
        return result;
    }

    // Collect valid hashes from DB (after phase 1)
    std::unordered_set<std::string> validHashes;
    {
        auto qHashes = db->query(
            "SELECT DISTINCT hash FROM entries WHERE hash IS NOT NULL AND hash <> ''");
        while (qHashes->fetch())
            validHashes.insert(qHashes->getText(0));
    }

    LOGD << "cleanupBuild: " << validHashes.size() << " valid hash(es) in DB; scanning "
         << io::toUtf8(buildDir);

    removeOrphanArtifacts(buildDir, validHashes, result);
    return result;
}

}  // namespace ddb
