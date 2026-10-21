/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Build dispatch: one build attempt of an entry (lock, scratch folder, the
// build of its EntryHandler, publish) and the buildAll/build entry points.

#include "build.h"

#include <cstdlib>
#include <fstream>
#include <optional>

#include "build_internal.h"
#include "build_layout.h"
#include "buildlock.h"
#include "dbops.h"
#include "deppath.h"
#include "entry_handler.h"
#include "exceptions.h"
#include "mio.h"
#include "raster_cache.h"

namespace ddb {

// Minimum free disk space (in MB) required on the build scratch filesystem
// before a build is started. Configurable via the DDB_MIN_FREE_DISK_MB
// environment variable; set it to 0 to disable the check. Defaults to 512 MB,
// low enough never to trip on a healthy host yet high enough to stop a build
// from beginning on an essentially full disk.
void ensureSufficientDiskSpace(const fs::path& dir) {
    long long minMb = 512;
    if (const char* env = std::getenv("DDB_MIN_FREE_DISK_MB"); env && *env) {
        char* endp = nullptr;
        const long long v = std::strtoll(env, &endp, 10);
        if (endp != env && v >= 0)
            minMb = v;
    }
    if (minMb == 0)
        return;  // explicitly disabled

    std::error_code ec;
    const fs::space_info si = fs::space(dir, ec);
    if (ec)
        return;  // cannot determine free space; do not block the build

    const std::uintmax_t minBytes =
        static_cast<std::uintmax_t>(minMb) * 1024ULL * 1024ULL;
    if (si.available < minBytes) {
        throw RetryableBuildException(
            "Insufficient free disk space to build: " +
            std::to_string(si.available / (1024ULL * 1024ULL)) + " MB available, " +
            std::to_string(minMb) + " MB required on " + dir.string() +
            ". Free up space or adjust DDB_MIN_FREE_DISK_MB.", RetryableBuildReason::LowDiskSpace);
    }
}

void buildInternal(Database* db, const Entry& e, const std::string& outputPath, bool force) {
    std::string outPath = outputPath;
    if (outPath.empty())
        outPath = db->buildDirectory().string();

    LOGD << "Building entry " << e.path << " type " << e.type;

    // The entry whose artifacts are built. Differs from e when e is a sidecar
    // (e.g. a .dbf) that triggers the build of its main file (the .shp): type
    // dispatch, the input path and the output hash must all come from the main file.
    const auto resolved = resolveBuildTarget(db, e);
    if (!resolved) {
        LOGD << "No build needed";
        return;
    }
    const Entry& target = resolved->entry;
    const std::string& subfolder = resolved->subfolder;
    const EntryHandler& handler = *resolved->handler;

    if (resolved->viaDependency) {
        // We need to force the build because this dependency file could add new data
        force = true;

        LOGD << "Triggering build of " << target.path << " because of " << e.path;
    }

    const fs::path baseOutputPath = fs::path(outPath) / target.hash;
    const std::string outputFolder = (baseOutputPath / subfolder).string();

    // Ensure the output directory structure exists before attempting to acquire the lock
    // This prevents BuildLockDirectoryException when multiple processes race to build
    io::assureFolderExists(baseOutputPath);

    // BuildLock excludes other threads of this process as well as other processes
    LOGD << "Acquiring build lock for: " << outputFolder;

    // BuildLock uses kernel-managed advisory locks (Windows: CreateFile no-share +
    // DELETE_ON_CLOSE; Linux: F_OFD_SETLK; macOS/BSD: flock). The kernel releases
    // the lock automatically when the holding process terminates, so orphan lock
    // files left by crashed processes are reclaimed transparently. A
    // BuildInProgressException therefore signals a *legitimately* active build by
    // another process - force=true must not override it, because doing so would
    // corrupt the in-progress output of the other process.
    BuildLock processLock(outputFolder);

    // Marker snapshot is read after exclusive primary ownership. Mutation waits
    // until serialized Model recovery has finished, so a busy tiles holder leaves
    // the original attempt timestamp untouched.
    const fs::path pendFile = pendingMarkerPath(baseOutputPath);
    bool wasPending = false;
    std::string pendingDependencyLines;

    const auto tempFolder = outputFolder + TempFolderInfix + utils::generateRandomString(16);
    auto cleanupTemporary = [&]() {
        try {
            io::assureIsRemoved(tempFolder);
        } catch (const std::exception& cleanupError) {
            LOGW << "Cannot remove build scratch '" << tempFolder << "': " << cleanupError.what();
        }
    };
    auto retainPending = [&]() {
        if (wasPending)
            replacePendingMarker(pendFile, pendingDependencyLines);
        cleanupTemporary();
    };

    try {
        wasPending = fs::is_regular_file(pendFile);
        if (wasPending) {
            std::ifstream pending(pendFile.string(), std::ios::binary);
            if (!pending.is_open())
                throw FSException("Cannot read pending marker " + pendFile.string());
            std::string timestamp;
            std::getline(pending, timestamp);
            pendingDependencyLines.assign(std::istreambuf_iterator<char>(pending), {});
        }

        // An interrupted publish (Model: 3D Tiles swap) is repaired under the
        // primary lock, before the completeness check.
        handler.recoverOutput(baseOutputPath);

        // Recheck primary artifacts after serialized recovery. Success consumes
        // pending work even when the output was already complete before this attempt.
        if (handler.isComplete(baseOutputPath, false) && !force) {
            LOGD << "Build output already exists after acquiring lock, skipping: " << outputFolder;
            io::assureIsRemoved(pendFile);
            return;
        }

        if (wasPending)
            replacePendingMarker(pendFile, pendingDependencyLines);
        // Both preflight and scratch creation are inside the classified attempt.
        ensureSufficientDiskSpace(baseOutputPath);
        io::assureFolderExists(tempFolder);

        const BuildContext context{db,
                                   target,
                                   db->rootDirectory() / target.path,
                                   db->rootDirectory(),
                                   baseOutputPath,
                                   fs::path(outputFolder),
                                   fs::path(tempFolder),
                                   force};
        handler.build(context);

        // A handler that publishes its own outputs (Vector) skips the standard rename
        if (handler.usesTempFolder()) {
            LOGD << "Build complete, moving temp folder to " << outputFolder;
            if (fs::exists(outputFolder)) {
                // Rasters of the previous output kept open would block this on Windows
                releaseCachedFiles(outputFolder);
                io::assureIsRemoved(outputFolder);
            }

            io::assureFolderExists(fs::path(outputFolder).parent_path());
            io::rename(tempFolder, outputFolder);
        }

        io::assureIsRemoved(tempFolder);
        io::assureIsRemoved(pendFile);
    } catch (const BuildDepMissingException& ex) {
        // Deferred success refreshes dependencies relative to the dataset root.
        // Atomic replacement also protects an existing marker if the disk is full.
        std::string dependencies;
        for (const auto& dep : ex.getMissingDependencies())
            dependencies += deppath::toDatasetRelative(target.path, dep) + "\n";
        replacePendingMarker(pendFile, dependencies);
        cleanupTemporary();
        throw;
    } catch (const BuildInProgressException&) {
        // Contention with the tiles holder is not an owned build attempt.
        cleanupTemporary();
        throw;
    } catch (const RetryableBuildException&) {
        retainPending();
        throw;
    } catch (const FSException& ex) {
        if (ex.code() == std::errc::no_space_on_device) {
            retainPending();
            throw RetryableBuildException(ex.what(), RetryableBuildReason::NoSpaceOnDevice);
        }
        io::assureIsRemoved(pendFile);
        cleanupTemporary();
        throw;
    } catch (const fs::filesystem_error& ex) {
        if (ex.code() == std::errc::no_space_on_device) {
            retainPending();
            throw RetryableBuildException(ex.what(), RetryableBuildReason::NoSpaceOnDevice);
        }
        io::assureIsRemoved(pendFile);
        cleanupTemporary();
        throw AppException(ex.what());
    } catch (const AppException&) {
        io::assureIsRemoved(pendFile);
        cleanupTemporary();
        throw;
    } catch (...) {
        // Unknown third-party failures are permanent, never inferred from text.
        io::assureIsRemoved(pendFile);
        cleanupTemporary();
        throw AppException("Unknown build error failure for " + target.path + " (" +
                           baseOutputPath.string() + ")");
    }
}

void buildAll(Database* db, const std::string& outputPath, bool force) {
    std::string outPath = outputPath;
    if (outPath.empty())
        outPath = db->buildDirectory().string();

    LOGD << "In buildAll('" << outputPath << "')";

    // List all entries of the buildable types in DB
    const std::vector<EntryType> types = buildableEntryTypes();
    if (types.empty())
        return;
    std::string placeholders;
    for (size_t i = 0; i < types.size(); i++)
        placeholders += i == 0 ? "?" : ", ?";
    auto q = db->query("SELECT path, hash, type, properties, mtime, size, depth FROM entries WHERE type IN (" +
                       placeholders + ")");
    for (size_t i = 0; i < types.size(); i++)
        q->bind(static_cast<int>(i + 1), static_cast<int>(types[i]));

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

}  // namespace ddb
