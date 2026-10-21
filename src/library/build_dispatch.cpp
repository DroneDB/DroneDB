/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Build dispatch: one build attempt of an entry (lock, scratch folder, the
// per-type builder, publish) and the buildAll/build entry points.

#include "build.h"

#include <cstdlib>
#include <fstream>
#include <optional>

#include "3d.h"
#include "build_internal.h"
#include "build_layout.h"
#include "buildlock.h"
#include "cog.h"
#include "dbops.h"
#include "deppath.h"
#include "exceptions.h"
#include "gsplat.h"
#include "mio.h"
#include "mzip.h"
#include "pointcloud.h"
#include "raster_cache.h"
#include "vector.h"

namespace ddb {

// Minimum free disk space (in MB) required on the build scratch filesystem
// before a build is started. Configurable via the DDB_MIN_FREE_DISK_MB
// environment variable; set it to 0 to disable the check. Defaults to 512 MB,
// low enough never to trip on a healthy host yet high enough to stop a build
// from beginning on an essentially full disk.
static void ensureSufficientDiskSpace(const fs::path& dir) {
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

    const std::string datasetRoot = db->rootDirectory().string();
    auto relativePath = (db->rootDirectory() / target.path).string();

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

        // Lock order is primary Model lock then tiles lock. Recovery must run
        // before the complete Nexus return, without recursively taking tiles lock.
        if (target.type == EntryType::Model)
            recoverModel3DTilesSwap((baseOutputPath / Tiles3DBuildSubfolder).string());

        // Recheck primary artifacts after serialized recovery. Success consumes
        // pending work even when Nexus was already complete before this attempt.
        if (isBuildOutputComplete(baseOutputPath, target, subfolder, false) && !force) {
            LOGD << "Build output already exists after acquiring lock, skipping: " << outputFolder;
            io::assureIsRemoved(pendFile);
            return;
        }

        if (wasPending)
            replacePendingMarker(pendFile, pendingDependencyLines);
        // Both preflight and scratch creation are inside the classified attempt.
        ensureSufficientDiskSpace(baseOutputPath);
        io::assureFolderExists(tempFolder);

        bool built = false;

        if (target.type == EntryType::PointCloud) {
            const std::vector vec = {relativePath};
            // Defensive cleanup: remove any legacy EPT artifacts from previous builds
            io::assureIsRemoved(baseOutputPath / LegacyEptBuildSubfolder);
            buildCopc(vec, tempFolder);
            built = true;
        } else if (target.type == EntryType::GeoRaster) {
            buildCog(relativePath, (fs::path(tempFolder) / CogFileName).string());
            built = true;
        } else if (target.type == EntryType::Model) {
            buildNexus(relativePath, (fs::path(tempFolder) / NexusFileName).string(), false,
                       datasetRoot);
            built = true;

            // Additive, best-effort OGC 3D Tiles output for the unified Giro3D viewer.
            // Written directly to a sibling 3dtiles/ folder (buildModel3DTiles performs
            // its own atomic temp+rename), exactly as buildVector co-produces vec/+mvt/.
            // Non-blocking by design: if Obj2Tiles is unavailable or fails on a given
            // mesh, the Nexus output above is still published and build completeness
            // stays keyed on nxs/ (see isBuildOutputComplete), so the build never loops.
            // Nothing is rethrown from here, BuildDepMissingException included: it cannot
            // occur (buildNexus validated the same input above), and rethrowing it would
            // discard tempFolder before model.nxz reaches nxs/.
            // Lock order is the primary lock (held here) then the "3dtiles" lock.
            // cleanupBuild follows the same order, so the only way to meet a busy
            // tiles lock is a tiles build started outside a Model build (the
            // "ddb tiles3d" command on this output folder).
            try {
                buildModel3DTiles(relativePath, (baseOutputPath / Tiles3DBuildSubfolder).string(), true,
                                  std::nullopt, true, datasetRoot);
            } catch (const BuildInProgressException& tilesEx) {
                LOGW << "3D Tiles for " << target.path << " not generated: another tiles build holds "
                     << buildLockPath((baseOutputPath / Tiles3DBuildSubfolder).string())
                     << " (for example 'ddb tiles3d'): "
                     << tilesEx.what() << ". The Nexus output is published; rebuild with force to "
                     << "produce the tiles";
            } catch (const std::exception& tilesEx) {
                LOGW << "3D Tiles generation skipped for " << target.path << ": " << tilesEx.what();
            } catch (...) {
                LOGW << "3D Tiles generation skipped for " << target.path << " (unknown error)";
            }
        } else if (target.type == EntryType::Vector) {
            // buildVector manages its own atomic write to baseOutputPath/vec
            // and baseOutputPath/mvt; do NOT use the standard tempFolder rename.
            // Propagate the force flag so a partial build (e.g. vec/ present
            // but mvt/ missing) is fully rebuilt instead of being silently
            // treated as up-to-date inside buildVector.
            buildVector(relativePath, baseOutputPath.string(), force);
            // built stays false on purpose to skip the standard rename below.
        } else if (target.type == EntryType::GaussianSplat) {
            // Runs build-lod to produce model.rad + bounds.json (+ georef.json when known).
            // Throws a plain AppException if build-lod is unavailable: a missing tool is
            // not a missing dependency, so the build fails instead of staying pending.
            buildGsplat(relativePath, tempFolder);
            built = true;
        } else if (target.type == EntryType::Tiles3D) {
            // Extracts the .3tz (ZIP OGC 3D Tiles) into the 3dtiles/ subfolder. Extraction
            // is hardened against Zip-Slip / absolute paths in mzip::_extractAll. The
            // archive is only valid when tileset.json is present at its root; the temp
            // folder is then renamed onto build/{hash}/3dtiles/ by the shared block below.
            zip::extractAll(relativePath, tempFolder);
            if (!fileExistsAndNonEmpty(fs::path(tempFolder) / TilesetFileName))
                throw AppException("Invalid .3tz archive: tileset.json not found at root");
            built = true;
        }

        if (built) {
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

    // List all buildable files in DB
    auto q = db->query(
        "SELECT path, hash, type, properties, mtime, size, depth FROM entries WHERE type = ? OR "
        "type = ? OR type = ? OR type = ? OR type = ? OR type = ?");
    q->bind(1, PointCloud);
    q->bind(2, GeoRaster);
    q->bind(3, Model);
    q->bind(4, Vector);
    q->bind(5, GaussianSplat);
    q->bind(6, Tiles3D);

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
