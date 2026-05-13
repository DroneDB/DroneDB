/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "build.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#endif

#include "3d.h"
#include "buildlock.h"
#include "cog.h"
#include "dbops.h"
#include "ddb.h"
#include "exceptions.h"
#include "mio.h"
#include "pointcloud.h"
#include "threadlock.h"
#include "vector.h"

namespace ddb {

// Forward declaration for stale lock detection helper (defined later in this file)
static bool isLockFileStale(const std::string& lockFilePath);

bool isBuildableInternal(const Entry& e, std::string& subfolder) {
    if (e.type == EntryType::PointCloud) {
        // Special case: do not build if this entry is a leaf inside a built EPT dataset.
        // We keep this guard to skip residual ept-data folders from older builds.
        if (fs::path(e.path).parent_path().filename().string() == "ept-data")
            return false;

        subfolder = "copc";
        return true;
    } else if (e.type == EntryType::GeoRaster) {
        subfolder = "cog";
        return true;
    } else if (e.type == EntryType::Model) {
        subfolder = "nxs";
        return true;
    } else if (e.type == EntryType::Vector) {
        subfolder = "vec";
        return true;
    }

    return false;
}

bool isBuildableDependency(const Entry& e, std::string& mainFile, std::string& subfolder) {
    // Special case: some files are not buildable by themselves but should trigger a build even if
    // the build has already been done For example .cpg, .dbf, .prj, .shx files in a shapefile

    if (e.type == EntryType::Generic) {
        io::Path p(e.path);
        if (p.checkExtension({"cpg", "dbf", "prj", "shx"})) {
            subfolder = "vec";
            // Mainfile has got the same name but with shp extension
            mainFile = fs::path(e.path).filename().replace_extension(".shp").string();
            return true;
        }
    }

    return false;
}

bool isBuildable(Database* db, const std::string& path, std::string& subfolder) {
    Entry e;

    const bool entryExists = getEntry(db, path, e);
    if (!entryExists)
        throw InvalidArgsException(path + " is not a valid path in the database.");

    if (isBuildableInternal(e, subfolder))
        return true;

    std::string mainFile;

    if (isBuildableDependency(e, mainFile, subfolder))
        return true;

    // If we reach here, the entry is not buildable
    return false;
}

void buildInternal(Database* db, const Entry& e, const std::string& outputPath, bool force) {
    std::string outPath = outputPath;
    if (outPath.empty())
        outPath = db->buildDirectory().string();

    LOGD << "Building entry " << e.path << " type " << e.type;

    fs::path baseOutputPath;
    std::string outputFolder;
    std::string subfolder;

    if (isBuildableInternal(e, subfolder)) {
        baseOutputPath = fs::path(outPath) / e.hash;
        outputFolder = (baseOutputPath / subfolder).string();
    } else {
        std::string mainFile;

        if (isBuildableDependency(e, mainFile, subfolder)) {
            // Check if main file exists
            Entry mainEntry;
            if (!getEntry(db, mainFile, mainEntry)) {
                LOGD << e.path << " is a dependency of " << mainFile << " but it's missing";
                return;
            }

            // We need to force the build because this dependency file could add new data
            force = true;

            baseOutputPath = fs::path(outPath) / mainEntry.hash;
            outputFolder = (baseOutputPath / subfolder).string();

            LOGD << "Triggering build of " << mainFile << " because of " << e.path;
        } else {
            LOGD << "No build needed";
            return;
        }
    }

    // Ensure the output directory structure exists before attempting to acquire the lock
    // This prevents BuildLockDirectoryException when multiple processes race to build
    io::assureFolderExists(baseOutputPath);

    // Acquire inter-process lock to prevent race conditions between different processes
    // This must come BEFORE the ThreadLock to ensure proper ordering of lock acquisition
    LOGD << "Acquiring inter-process build lock for: " << outputFolder;

    // BuildLock uses kernel-managed advisory locks (Windows: CreateFile no-share +
    // DELETE_ON_CLOSE; Linux: F_OFD_SETLK; macOS/BSD: flock). The kernel releases
    // the lock automatically when the holding process terminates, so orphan lock
    // files left by crashed processes are reclaimed transparently. A
    // BuildInProgressException therefore signals a *legitimately* active build by
    // another process - force=true must not override it, because doing so would
    // corrupt the in-progress output of the other process.
    BuildLock processLock(outputFolder);

    // Acquire intra-process lock to coordinate between threads of the same process
    ThreadLock threadLock("build-" + (db->rootDirectory() / e.hash).string());

    // Check again if output exists after acquiring locks (another process might have completed the
    // build)
    if (fs::exists(outputFolder) && !force) {
        LOGD << "Build output already exists after acquiring lock, skipping: " << outputFolder;
        return;
    }

    const auto tempFolder = outputFolder + "-temp-" + utils::generateRandomString(16);

    io::assureFolderExists(tempFolder);

    auto relativePath = (db->rootDirectory() / e.path).string();

    std::string pendFile = baseOutputPath.string() + ".pending";
    io::assureIsRemoved(pendFile);

    // We could vectorize this logic, but it's an overkill by now
    try {
        bool built = false;

        if (e.type == EntryType::PointCloud) {
            const std::vector vec = {relativePath};
            // Defensive cleanup: remove any legacy EPT artifacts from previous builds
            io::assureIsRemoved(baseOutputPath / "ept");
            buildCopc(vec, tempFolder);
            built = true;
        } else if (e.type == EntryType::GeoRaster) {
            buildCog(relativePath, (fs::path(tempFolder) / "cog.tif").string());
            built = true;
        } else if (e.type == EntryType::Model) {
            buildNexus(relativePath, (fs::path(tempFolder) / "model.nxz").string());
            built = true;
        } else if (e.type == EntryType::Vector) {
            // buildVector manages its own atomic write to baseOutputPath/vec
            // and baseOutputPath/mvt; do NOT use the standard tempFolder rename.
            buildVector(relativePath, baseOutputPath.string());
            // built stays false on purpose to skip the standard rename below.
        }

        if (built) {
            LOGD << "Build complete, moving temp folder to " << outputFolder;
            if (fs::exists(outputFolder))
                io::assureIsRemoved(outputFolder);

            io::assureFolderExists(fs::path(outputFolder).parent_path());
            io::rename(tempFolder, outputFolder);
        }

        io::assureIsRemoved(tempFolder);
    } catch (const BuildDepMissingException& e) {
        // Create pending file with timestamp and missing dependencies
        std::ofstream pf(pendFile);
        if (pf) {
            // First line: timestamp
            pf << utils::currentUnixTimestamp() << std::endl;
            // Following lines: missing dependencies (one per line)
            for (const auto& dep : e.getMissingDependencies()) {
                pf << dep << std::endl;
            }
            pf.close();

            LOGD << "Created pending file for " << e.what() << " with "
                 << e.getMissingDependencies().size() << " missing dependencies";
        } else {
            LOGD << "Error! Cannot create pending file " << baseOutputPath.string() << ".pending";
        }

        io::assureIsRemoved(tempFolder);

        throw e;
    } catch (const AppException& e) {
        io::assureIsRemoved(tempFolder);

        throw e;
    } catch (...) {
        // Since we use third party libraries, some exceptions might not
        // get caught otherwise
        io::assureIsRemoved(tempFolder);

        throw AppException("Unknown build error failure for " + e.path + " (" +
                           baseOutputPath.string() + ")");
    }
}

void buildAll(Database* db, const std::string& outputPath, bool force) {
    std::string outPath = outputPath;
    if (outPath.empty())
        outPath = db->buildDirectory().string();

    LOGD << "In buildAll('" << outputPath << "')";

    // List all buildable files in DB
    auto q = db->query(
        "SELECT path, hash, type, properties, mtime, size, depth FROM entries WHERE type = ? OR "
        "type = ? OR type = ? OR type = ?");
    q->bind(1, PointCloud);
    q->bind(2, GeoRaster);
    q->bind(3, Model);
    q->bind(4, Vector);

    while (q->fetch()) {
        Entry e(q->getText(0),
                q->getText(1),
                q->getInt(2),
                q->getText(3),
                q->getInt64(4),
                q->getInt64(5),
                q->getInt(6));

        // Call build on each of them
        try {
            buildInternal(db, e, outPath, force);
        } catch (const AppException& err) {
            LOGD << "Cannot build " << e.path << ": " << err.what();
        }
    }
}

void build(Database* db, const std::string& path, const std::string& outputPath, bool force) {
    LOGD << "In build('" << path << "','" << outputPath << "')";

    Entry e;

    const bool entryExists = getEntry(db, path, e);
    if (!entryExists)
        throw InvalidArgsException(path + " is not a valid path in the database.");

    buildInternal(db, e, outputPath, force);
}

void buildPending(Database* db, const std::string& outputPath, bool force) {
    auto buildDir = db->buildDirectory();
    if (!fs::exists(buildDir))
        return;

    std::string outPath = outputPath;
    if (outPath.empty())
        outPath = buildDir.string();

    for (auto i = fs::recursive_directory_iterator(buildDir);
         i != fs::recursive_directory_iterator();
         ++i) {
        if (i->path().extension() == ".pending") {
            auto hash = i->path().filename().replace_extension("").string();

            // Check if file still exists in our index
            auto q = db->query(
                "SELECT path, hash, type, properties, mtime, size, depth FROM entries WHERE hash = "
                "?");
            q->bind(1, hash);
            bool found = false;

            // Read the pending file to get missing dependencies
            std::vector<std::string> missingDependencies;
            std::ifstream pendingFile(i->path().string());
            if (pendingFile) {
                std::string line;
                // First line is the timestamp                    std::getline(pendingFile, line);
                time_t lastAttempt = 0;
                try {
                    // Validate timestamp format before conversion
                    for (char c : line)
                        if (!std::isdigit(c) && c != '-' && c != '+')
                            throw std::invalid_argument("Invalid timestamp format: " + line);

                    lastAttempt = std::stoll(line);

                    // Basic validation - timestamp should be reasonable
                    time_t currentTime = utils::currentUnixTimestamp();
                    if (lastAttempt > currentTime)
                        LOGW << "Timestamp in pending file is in the future: " << line;
                    if (lastAttempt < 0)
                        throw std::invalid_argument("Negative timestamp");
                } catch (const std::invalid_argument& e) {
                    LOGD << "Invalid timestamp format in pending file: " << e.what();
                } catch (const std::out_of_range& e) {
                    LOGD << "Timestamp out of range in pending file: " << e.what();
                } catch (...) {
                    LOGD << "Unknown error parsing timestamp in pending file";
                }

                // Implement exponential backoff for retry attempts
                time_t currentTime = utils::currentUnixTimestamp();
                time_t timeSinceLastAttempt = currentTime - lastAttempt;

                // Check pending file age to implement progressive backoff
                // If recent failure (<5 min), wait longer before retry unless forced
                if (timeSinceLastAttempt < 300 && !force) {  // 5 minutes
                    LOGD << "Skipping build attempt for hash " << hash
                         << " (too recent failure: " << timeSinceLastAttempt << " seconds ago)";
                    continue;
                }

                // Read the rest as dependencies
                while (std::getline(pendingFile, line)) {
                    if (!line.empty()) {
                        missingDependencies.push_back(line);
                    }
                }
                pendingFile.close();
            }

            // Check if all dependencies are now available
            bool allDependenciesAvailable = true;
            for (const auto& dep : missingDependencies) {
                // Look for dependency in database
                auto depQuery = db->query("SELECT COUNT(*) FROM entries WHERE path = ?");
                depQuery->bind(1, dep);
                if (depQuery->fetch() && depQuery->getInt(0) == 0) {
                    // Dependency still not available
                    allDependenciesAvailable = false;
                    LOGD << "Build still pending for hash " << hash << ": dependency " << dep
                         << " is still missing";
                    break;
                }
            }

            // Only proceed with the build if all dependencies are available or if forced
            if (!allDependenciesAvailable && !force) {
                LOGD << "Skipping build attempt for hash " << hash
                     << " due to missing dependencies";
                continue;
            }

            while (q->fetch()) {
                found = true;
                Entry e(q->getText(0),
                        q->getText(1),
                        q->getInt(2),
                        q->getText(3),
                        q->getInt64(4),
                        q->getInt64(5),
                        q->getInt(6));

                // Only remove the pending file if we're going to attempt the build
                io::assureIsRemoved(i->path());

                // Call build
                try {
                    LOGD << "Attempting build for " << e.path
                         << " (all dependencies now available)";
                    buildInternal(db, e, outPath, force);
                } catch (const AppException& err) {
                    LOGD << "Cannot build " << e.path << ": " << err.what();
                }
            }

            if (!found)
                io::assureIsRemoved(i->path());
        }
    }
}

bool isBuildPending(Database* db) {
    auto buildDir = db->buildDirectory();
    if (!fs::exists(buildDir))
        return false;

    for (auto i = fs::recursive_directory_iterator(buildDir);
         i != fs::recursive_directory_iterator();
         ++i) {
        if (i->path().extension() == ".pending")
            return true;
    }

    return false;
}

CleanupResult cleanupBuild(Database* db, const std::string& outputPath) {
    CleanupResult result;

    // -------- Phase 1: remove DB entries whose underlying file is gone --------
    const fs::path rootDir = db->rootDirectory();
    std::vector<std::string> missingPaths;

    {
        auto qEntries = db->query(
            "SELECT path FROM entries WHERE type <> ? ORDER BY path");
        qEntries->bind(1, EntryType::Directory);
        while (qEntries->fetch()) {
            const std::string entryPath = qEntries->getText(0);
            std::error_code existsErr;
            const bool present = fs::exists(rootDir / entryPath, existsErr);
            if (existsErr) {
                LOGW << "Cannot stat '" << entryPath
                     << "' (skipping): " << existsErr.message();
                continue;
            }
            if (!present)
                missingPaths.push_back(entryPath);
        }
    }

    if (!missingPaths.empty()) {
        LOGD << "cleanupBuild: removing " << missingPaths.size()
             << " stale DB entry/entries";
        // Remove entries one at a time so that a failure on a single path
        // does not lose the record of paths that were actually removed.
        // removeFromIndex expects absolute paths (it computes the relative
        // form internally via io::Path::relativeTo).
        for (const auto& rel : missingPaths) {
            const std::string absPath = (rootDir / rel).string();
            try {
                removeFromIndex(db, std::vector<std::string>{absPath});
                result.removedEntries.push_back(rel);
            } catch (const AppException& e) {
                LOGW << "Failed to remove stale DB entry '" << rel
                     << "': " << e.what();
            }
        }
    }

    // -------- Phase 2: remove orphan build artifacts --------
    const fs::path buildDir = outputPath.empty() ? db->buildDirectory() : fs::path(outputPath);
    std::error_code dirExistsErr;
    if (!fs::exists(buildDir, dirExistsErr)) {
        if (dirExistsErr)
            LOGW << "Cannot check build directory '" << buildDir.string()
                 << "': " << dirExistsErr.message();
        else
            LOGD << "Build directory does not exist: " << buildDir.string();
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
         << buildDir.string();

    // Build-artifact names are content hashes (currently SHA-256 hex, 64 chars).
    // Validate the candidate name before deleting so we never wipe unrelated
    // files/directories that may live in a user-specified output path.
    auto isHashLike = [](const std::string& s) {
        if (s.size() != 64)
            return false;
        for (unsigned char c : s) {
            if (!std::isxdigit(c))
                return false;
        }
        return true;
    };

    std::error_code iterErr;
    fs::directory_iterator it(buildDir, iterErr);
    if (iterErr) {
        LOGW << "Cannot open build directory '" << buildDir.string()
             << "': " << iterErr.message();
        return result;
    }

    const fs::directory_iterator end;
    for (; it != end; it.increment(iterErr)) {
        if (iterErr) {
            LOGW << "Error iterating build directory '" << buildDir.string()
                 << "': " << iterErr.message();
            break;
        }

        const auto& entry = *it;
        const auto& p = entry.path();
        const std::string name = p.filename().string();

        std::error_code typeErr;
        if (entry.is_directory(typeErr)) {
            // Directory name is expected to be a content hash. Skip anything
            // that doesn't look like one to avoid clobbering unrelated dirs.
            if (!isHashLike(name)) {
                LOGD << "Skipping non-hash directory: " << p.string();
                continue;
            }
            if (validHashes.find(name) != validHashes.end())
                continue;  // still referenced

            // Lock files live one level deep: <hash>/<subfolder>.building.
            // Use a non-recursive scan and treat any iteration error as
            // "potentially active" to avoid wrongly deleting a locked dir.
            bool skip = false;
            std::error_code subErr;
            fs::directory_iterator subIt(p, subErr);
            if (subErr) {
                LOGW << "Cannot scan orphan '" << p.string()
                     << "' for active locks (" << subErr.message()
                     << "); skipping removal as a precaution";
                skip = true;
            } else {
                const fs::directory_iterator subEnd;
                for (; !skip && subIt != subEnd; subIt.increment(subErr)) {
                    if (subErr) {
                        LOGW << "Iteration error inside '" << p.string()
                             << "': " << subErr.message()
                             << "; skipping removal as a precaution";
                        skip = true;
                        break;
                    }
                    if (subIt->path().extension() == ".building" &&
                        !isLockFileStale(subIt->path().string())) {
                        LOGD << "Skipping orphan with active build lock: " << p.string();
                        skip = true;
                        break;
                    }
                }
            }

            if (skip)
                continue;

            std::error_code rmErr;
            fs::remove_all(p, rmErr);
            if (rmErr) {
                LOGW << "Failed to remove orphan build directory '" << p.string()
                     << "': " << rmErr.message();
            } else {
                result.removedBuilds.push_back(p.string());
                LOGD << "Removed orphan build directory: " << p.string();
            }
        } else if (entry.is_regular_file(typeErr) && p.extension() == ".pending") {
            // .pending file naming convention: <hash>.pending. Validate the
            // stem before deleting so we never touch unrelated *.pending files.
            const std::string hash = p.stem().string();
            if (!isHashLike(hash)) {
                LOGD << "Skipping non-hash pending file: " << p.string();
                continue;
            }
            if (validHashes.find(hash) != validHashes.end())
                continue;

            std::error_code rmErr;
            fs::remove(p, rmErr);
            if (rmErr) {
                LOGW << "Failed to remove orphan pending file '" << p.string()
                     << "': " << rmErr.message();
            } else {
                result.removedBuilds.push_back(p.string());
                LOGD << "Removed orphan pending file: " << p.string();
            }
        }
    }

    return result;
}

// Helper function for stale lock detection.
//
// Background: a ".building" file may persist on disk after a crash, but the
// OS-level lock held by the dead process is gone (the kernel releases it
// automatically). The most reliable way to tell "is anyone really holding
// this lock?" is to attempt a non-blocking acquisition - if it succeeds the
// previous holder is dead (stale), if it throws BuildInProgressException the
// holder is alive. We immediately release the lock we just took (via RAII)
// without removing the file, so a subsequent legitimate caller of BuildLock
// gets a clean state.

/**
 * @brief Check if a build lock file is stale (no live process holds the lock)
 * @param lockFilePath Path to the .building lock file
 * @return true if lock is stale (and can be safely removed), false if lock is
 *         still held by an active process or status is uncertain
 */
static bool isLockFileStale(const std::string& lockFilePath) {
    if (!fs::exists(lockFilePath)) {
        return false;  // Nothing to check
    }

#ifdef _WIN32
    // On Windows the BuildLock implementation holds the file with no sharing
    // (dwShareMode = 0) and FILE_FLAG_DELETE_ON_CLOSE. So if we can open it
    // exclusively for read with OPEN_EXISTING, no process currently owns the
    // lock -> it is stale and the file can be removed safely. We do not pass
    // FILE_FLAG_DELETE_ON_CLOSE here: cleanupBuild will remove the parent
    // directory itself.
    HANDLE h = CreateFileA(
        lockFilePath.c_str(),
        GENERIC_READ,
        0,  // No sharing - matches the lock holder's request
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED) {
            LOGD << "Lock file is valid (live holder exists): " << lockFilePath;
            return false;
        }
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return false;  // Race: file vanished
        }
        LOGD << "Cannot determine Windows lock status (err " << err << "), assuming valid: " << lockFilePath;
        return false;
    }
    CloseHandle(h);
    LOGD << "Lock file is stale (no live holder on Windows): " << lockFilePath;
    return true;
#else
    // The lock file path is "<outputFolder>.building"; BuildLock appends the
    // ".building" suffix internally, so we must strip it before constructing.
    const std::string suffix = ".building";
    if (lockFilePath.size() <= suffix.size() ||
        lockFilePath.compare(lockFilePath.size() - suffix.size(), suffix.size(), suffix) != 0) {
        LOGD << "Unexpected lock file name (no .building suffix): " << lockFilePath;
        return false;
    }
    const std::string outputFolder =
        lockFilePath.substr(0, lockFilePath.size() - suffix.size());

    try {
        // Non-blocking acquisition; success means the previous holder is dead.
        BuildLock probe(outputFolder, false);
        LOGD << "Lock file is stale (kernel reports no live holder): " << lockFilePath;
        // RAII releases the probe lock and unlinks the file on destruction.
        // The caller of isLockFileStale (cleanupBuild) expects the file to be
        // safely removable, which it now is.
        return true;
    } catch (const BuildInProgressException&) {
        LOGD << "Lock file is valid (live holder exists): " << lockFilePath;
        return false;
    } catch (const AppException& e) {
        LOGD << "Cannot determine lock status (" << e.what() << "), assuming valid";
        return false;
    }
#endif
}

bool isBuildActive(Database* db, const std::string& path) {
    Entry e;
    const bool entryExists = getEntry(db, path, e);
    if (!entryExists)
        return false;

    std::string subfolder;
    if (!isBuildableInternal(e, subfolder)) {
        std::string mainFile;
        if (!isBuildableDependency(e, mainFile, subfolder))
            return false;

        // For dependencies, check if main file exists and use its hash
        Entry mainEntry;
        if (!getEntry(db, mainFile, mainEntry))
            return false;

        e = mainEntry;
    }

    // Construct the output folder path
    std::string outPath = db->buildDirectory().string();
    fs::path baseOutputPath = fs::path(outPath) / e.hash;
    std::string outputFolder = (baseOutputPath / subfolder).string();
    std::string lockFile = outputFolder + ".building";

    LOGD << "Checking for active build in: " << outputFolder;

    if (!fs::exists(lockFile)) {
        return false;  // No lock file means no active build
    }

    // Reuse the cross-platform stale detection. If the lock file is stale
    // (no kernel-level holder), clean it up and report no active build.
    if (isLockFileStale(lockFile)) {
        LOGD << "Removing stale lock file: " << lockFile;
        std::error_code ec;
        fs::remove(lockFile, ec);
        if (ec) {
            LOGD << "Failed to remove stale lock file (" << ec.message() << "): " << lockFile;
        }
        return false;
    }

    LOGD << "Active build detected (kernel lock held): " << lockFile;
    return true;
}

}  // namespace ddb
