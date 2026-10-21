/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "build.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#endif

#include "3d.h"
#include "build_layout.h"
#include "buildlock.h"
#include "cog.h"
#include "dbops.h"
#include "ddb.h"
#include "deppath.h"
#include "exceptions.h"
#include "gsplat.h"
#include "raster_cache.h"
#include "mio.h"
#include "mzip.h"
#include "pointcloud.h"
#include "vector.h"

namespace ddb {

// Forward declaration for the disk-space preflight helper (defined later in this file)
static void ensureSufficientDiskSpace(const fs::path& dir);

// Write the complete marker into a sibling file before replacing the old one.
// Failure (especially ENOSPC) must never remove the last pending marker.
static bool replacePendingMarker(const fs::path& pendingPath, const std::string& dependencyLines) {
    // "<hash>.pending.tmp-<rand16>", see pendingMarkerTempInfix().
    const fs::path temporary =
        pendingPath.string() + PendingMarkerTempTag + utils::generateRandomString(16);
    try {
        std::ofstream out;
        errno = 0;
        out.open(temporary.string(), std::ios::binary | std::ios::trunc);
        const int openError = errno;
        if (!out.is_open())
            throw FSException("Cannot open pending marker " + temporary.string(),
                              std::error_code(openError, std::generic_category()));
        errno = 0;
        out << utils::currentUnixTimestamp() << "\n" << dependencyLines;
        out.flush();
        const int writeError = errno;
        if (!out)
            throw FSException("Cannot write pending marker " + temporary.string(),
                              std::error_code(writeError, std::generic_category()));
        errno = 0;
        out.close();
        const int closeError = errno;
        if (out.fail())
            throw FSException("Cannot close pending marker " + temporary.string(),
                              std::error_code(closeError, std::generic_category()));
#ifdef _WIN32
        // std::filesystem::rename does not replace an existing file on Windows.
        if (!MoveFileExW(temporary.c_str(), pendingPath.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const std::error_code ec(GetLastError(), std::system_category());
            throw FSException("Cannot replace pending marker: " + ec.message(), ec);
        }
#else
        io::rename(temporary, pendingPath);
#endif
        return true;
    } catch (const std::exception& ex) {
        LOGW << "Cannot refresh pending marker '" << pendingPath.string()
             << "'; original marker retained: " << ex.what();
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
}

// The sibling written by replacePendingMarker() before the atomic rename is
// "<hash>.pending.tmp-<rand16>" (pendingMarkerTempInfix()). A crash between write
// and rename leaves it behind; cleanupBuild removes such leftovers once they are old.
// True for "<64 hex>.pending.tmp-<non-empty suffix>".
static bool isPendingMarkerTempName(const std::string& name) {
    const std::string infix = pendingMarkerTempInfix();
    const size_t infixAt = 64;
    if (name.size() <= infixAt + infix.size())
        return false;
    if (name.compare(infixAt, infix.size(), infix) != 0)
        return false;
    return isHashLike(name.substr(0, infixAt));
}

// A pending marker temp file younger than this may belong to a live
// replacePendingMarker() call (its rename follows the close within
// microseconds); only older leftovers are collected.
static constexpr std::chrono::hours PendingMarkerTempMinAge{1};

bool isBuildableInternal(const Entry& e, std::string& subfolder) {
    if (e.type == EntryType::PointCloud) {
        // Special case: do not build if this entry is a leaf inside a built EPT dataset.
        // We keep this guard to skip residual ept-data folders from older builds.
        if (fs::path(e.path).parent_path().filename().string() == LegacyEptDataFolder)
            return false;

        subfolder = CopcBuildSubfolder;
        return true;
    } else if (e.type == EntryType::GeoRaster) {
        subfolder = CogBuildSubfolder;
        return true;
    } else if (e.type == EntryType::Model) {
        subfolder = NexusBuildSubfolder;
        return true;
    } else if (e.type == EntryType::Vector) {
        subfolder = VectorBuildSubfolder;
        return true;
    } else if (e.type == EntryType::GaussianSplat) {
        subfolder = GsplatBuildSubfolder;
        return true;
    } else if (e.type == EntryType::Tiles3D) {
        subfolder = Tiles3DBuildSubfolder;
        return true;
    }

    return false;
}

using EntryLookup = std::function<bool(const std::string&, Entry&)>;

// Entry paths are dataset-relative, so the folder is kept
std::vector<std::string> sidecarMainCandidates(const std::string& path) {
    io::Path p(path);
    std::vector<const char*> extensions;
    if (p.checkExtension({"cpg", "dbf", "shx"}))
        extensions = {".shp", ".SHP"};
    else if (p.checkExtension({"prj"}))
        extensions = {".shp", ".SHP", ".dxf", ".DXF", ".dwg", ".DWG"};
    else
        return {};

    std::vector<std::string> candidates;
    for (const char* ext : extensions)
        candidates.push_back(fs::path(path).replace_extension(ext).generic_string());
    return candidates;
}

static std::vector<std::string> dependencyMainCandidates(const Entry& e) {
    if (e.type != EntryType::Generic)
        return {};
    return sidecarMainCandidates(e.path);
}

// Special case: some files are not buildable by themselves but trigger the
// (forced) rebuild of their main file, e.g. the .dbf of a shapefile or the .prj
// of a drawing. A sidecar is buildable only while its main file is indexed:
// an orphan .prj next to a raster is not (otherwise it is never "complete"
// and is rebuilt forever by completeness sweeps).
static bool isBuildableDependency(const Entry& e, const EntryLookup& lookup, Entry& mainEntry,
                                  std::string& subfolder) {
    for (const auto& candidate : dependencyMainCandidates(e)) {
        if (lookup(candidate, mainEntry) && isBuildableInternal(mainEntry, subfolder))
            return true;
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

    Entry mainEntry;
    return isBuildableDependency(e, [db](const std::string& p, Entry& out) { return getEntry(db, p, out); },
                                 mainEntry, subfolder);
}

// Returns true if the given file exists and has non-zero size.
static bool fileExistsAndNonEmpty(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec) || ec)
        return false;
    if (!fs::is_regular_file(p, ec) || ec)
        return false;
    const auto sz = fs::file_size(p, ec);
    if (ec)
        return false;
    return sz > 0;
}

// Returns true if the given directory exists and contains at least one
// regular file (recursively) with non-zero size. Used to validate that
// a build output folder is not merely an empty stub left by an aborted build.
static bool directoryHasNonEmptyContent(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec) || ec)
        return false;
    if (!fs::is_directory(dir, ec) || ec)
        return false;

    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        const auto& entry = *it;
        std::error_code fec;
        if (entry.is_regular_file(fec) && !fec) {
            const auto sz = entry.file_size(fec);
            if (!fec && sz > 0)
                return true;
        }
    }

    return false;
}

// Centralized completeness check for a buildable entry's output. Validates
// that the actual artifacts produced by each build type exist and have
// non-zero content. Empty folders (e.g. left over from a crashed build)
// are reported as incomplete so the build can be re-queued.
//
// Per-type rules:
//   - Vector:    vec/source.gpkg AND mvt/metadata.json both exist and non-empty.
//                Checks two specific canonical sentinel files rather than
//                scanning the directory; this is intentional - both artifacts
//                are always co-produced and uniquely identify a complete build.
//                A local CAD drawing (no CRS) has no MVT: vec/local.json
//                replaces mvt/metadata.json as the second sentinel.
//   - Others:    the type's output subfolder exists and contains at least one
//                non-empty file (covers copc/, cog/, nxs/). A directory-scan
//                is used because the exact output filenames vary by input.
//
// Model note: a Model build also co-produces an additive OGC 3D Tiles tileset in
// a sibling 3dtiles/ folder (see buildInternal). That artifact is intentionally
// optional for completeness. The sole exception is an identifiable interrupted
// tiles swap (usable backup and missing/incomplete final); report it incomplete
// so the normal completeness sweep can enqueue recovery without rebuilding nxs/.
static bool isBuildOutputComplete(const fs::path& baseOutputPath,
                                  const Entry& e,
                                  const std::string& subfolder,
                                  bool checkTilesSwap = true) {
    if (e.type == EntryType::Vector)
        return fileExistsAndNonEmpty(baseOutputPath / VectorBuildSubfolder / VectorGpkgFileName) &&
               (fileExistsAndNonEmpty(baseOutputPath / MvtBuildSubfolder / MvtMetadataFileName) ||
                fileExistsAndNonEmpty(baseOutputPath / VectorBuildSubfolder / VectorLocalFileName));

    if (e.type == EntryType::GaussianSplat)
        return fileExistsAndNonEmpty(baseOutputPath / GsplatBuildSubfolder / GsplatRadFileName);

    if (e.type == EntryType::Tiles3D)
        return fileExistsAndNonEmpty(baseOutputPath / Tiles3DBuildSubfolder / TilesetFileName);

    if (checkTilesSwap && e.type == EntryType::Model &&
        hasRecoverableModel3DTilesSwap(baseOutputPath / Tiles3DBuildSubfolder))
        return false;

    return directoryHasNonEmptyContent(baseOutputPath / subfolder);
}

// isBuildComplete() for an entry already read from the index. lookup reads the
// main file of a buildable dependency (e.g. the .shp of a .dbf).
static bool isEntryBuildComplete(const fs::path& buildDirectory, const Entry& entry,
                                 const EntryLookup& lookup) {
    Entry e = entry;
    std::string subfolder;
    if (!isBuildableInternal(e, subfolder)) {
        // For dependencies, completeness is defined by the main file's artifacts.
        Entry mainEntry;
        if (!isBuildableDependency(e, lookup, mainEntry, subfolder))
            return false;
        e = mainEntry;
    }

    return isBuildOutputComplete(buildDirectory / e.hash, e, subfolder);
}

bool isBuildComplete(Database* db, const std::string& path) {
    Entry e;
    if (!getEntry(db, path, e))
        return false;

    return isEntryBuildComplete(db->buildDirectory(), e, [db](const std::string& p, Entry& out) {
        return getEntry(db, p, out);
    });
}

std::vector<BuildStatus> getBuildStatus(Database* db, const std::vector<std::string>& paths) {
    // One prepared statement for every lookup; only the columns the build rules read
    auto q = db->query("SELECT path, hash, type FROM entries WHERE path = ?");
    const auto lookup = [&q](const std::string& p, Entry& out) {
        q->bind(1, p);
        const bool found = q->fetch();
        if (found) {
            out.path = q->getText(0);
            out.hash = q->getText(1);
            out.type = static_cast<EntryType>(q->getInt(2));
        }
        q->reset();
        return found;
    };

    const fs::path buildDirectory = db->buildDirectory();
    std::vector<BuildStatus> result;
    result.reserve(paths.size());

    for (const auto& path : paths) {
        BuildStatus status;
        status.path = path;

        Entry e;
        if (lookup(path, e)) {
            status.exists = true;

            std::string subfolder;
            Entry mainEntry;
            status.buildable = isBuildableInternal(e, subfolder) ||
                               isBuildableDependency(e, lookup, mainEntry, subfolder);
            status.complete = status.buildable && isEntryBuildComplete(buildDirectory, e, lookup);
        }

        result.push_back(std::move(status));
    }

    return result;
}

void buildInternal(Database* db, const Entry& e, const std::string& outputPath, bool force) {
    std::string outPath = outputPath;
    if (outPath.empty())
        outPath = db->buildDirectory().string();

    LOGD << "Building entry " << e.path << " type " << e.type;

    fs::path baseOutputPath;
    std::string outputFolder;
    std::string subfolder;

    // The entry whose artifacts are built. Differs from e when e is a sidecar
    // (e.g. a .dbf) that triggers the build of its main file (the .shp): type
    // dispatch, the input path and the output hash must all come from the main file.
    Entry target = e;

    if (isBuildableInternal(e, subfolder)) {
        baseOutputPath = fs::path(outPath) / e.hash;
        outputFolder = (baseOutputPath / subfolder).string();
    } else {
        Entry mainEntry;

        if (isBuildableDependency(e, [db](const std::string& p, Entry& out) { return getEntry(db, p, out); },
                                  mainEntry, subfolder)) {
            // We need to force the build because this dependency file could add new data
            force = true;

            target = mainEntry;
            baseOutputPath = fs::path(outPath) / target.hash;
            outputFolder = (baseOutputPath / subfolder).string();

            LOGD << "Triggering build of " << target.path << " because of " << e.path;
        } else {
            LOGD << "No build needed";
            return;
        }
    }

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

namespace {

// Parsed contents of a ".pending" marker file created by buildInternal() when
// a build is deferred due to missing dependencies (see BuildDepMissingException
// handling above). Format: first line is a Unix timestamp of the last
// attempt, following lines are missing dependency names (one per line).
// Names are dataset-root-relative paths of companion files (e.g. "sub/m.mtl",
// "sub/mats/tex.png"); markers written by older versions may hold names
// relative to the source file's folder. They are informational only: a retry
// never matches them against the index.
struct PendingFileData {
    time_t lastAttempt = 0;
    std::vector<std::string> missingDependencies;
};

// Reads and parses a ".pending" file without consuming it. Malformed or
// unreadable timestamps are tolerated (lastAttempt stays 0, i.e. the build is
// due immediately) to preserve the historical, lenient behavior of buildPending().
PendingFileData parsePendingFile(const fs::path& pendingFilePath) {
    PendingFileData data;

    std::ifstream pendingFile(pendingFilePath.string());
    if (!pendingFile)
        return data;

    std::string line;
    // First line is the timestamp
    std::getline(pendingFile, line);
    try {
        // Validate timestamp format before conversion.
        // std::isdigit requires a value representable as unsigned char: a plain (signed)
        // char holding a non-ASCII byte from a corrupted .pending file would be UB.
        for (char c : line)
            if (!std::isdigit(static_cast<unsigned char>(c)) && c != '-' && c != '+')
                throw std::invalid_argument("Invalid timestamp format: " + line);

        data.lastAttempt = std::stoll(line);

        // Basic validation - timestamp should be reasonable
        const time_t currentTime = utils::currentUnixTimestamp();
        if (data.lastAttempt > currentTime)
            LOGW << "Timestamp in pending file is in the future: " << line;
        if (data.lastAttempt < 0)
            throw std::invalid_argument("Negative timestamp");
    } catch (const std::invalid_argument& e) {
        LOGD << "Invalid timestamp format in pending file: " << e.what();
        data.lastAttempt = 0;
    } catch (const std::out_of_range& e) {
        LOGD << "Timestamp out of range in pending file: " << e.what();
        data.lastAttempt = 0;
    } catch (...) {
        LOGD << "Unknown error parsing timestamp in pending file";
        data.lastAttempt = 0;
    }

    // Read the rest as dependencies
    while (std::getline(pendingFile, line)) {
        if (!line.empty())
            data.missingDependencies.push_back(line);
    }

    return data;
}

// Collects the ".pending" markers of a build directory: only top-level
// "<hash>.pending" regular files (buildInternal writes them next to the per-hash
// output folders), never nested or non-hash names. Collecting first also keeps
// callers that build from mutating the directory under a live iterator.
// Iteration errors are logged and end the scan (never thrown).
std::vector<fs::path> collectPendingFiles(const fs::path& buildDir) {
    std::vector<fs::path> pendingFiles;
    std::error_code ec;
    if (!fs::exists(buildDir, ec) || ec)
        return pendingFiles;

    fs::directory_iterator it(buildDir, ec);
    if (ec) {
        LOGW << "Cannot open build directory '" << buildDir.string() << "': " << ec.message();
        return pendingFiles;
    }
    for (const fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) {
            LOGW << "Error iterating build directory '" << buildDir.string() << "': " << ec.message();
            break;
        }
        const fs::path& p = it->path();
        std::error_code typeErr;
        if (p.extension() != PendingMarkerExtension || !it->is_regular_file(typeErr) || typeErr)
            continue;
        if (!isHashLike(p.stem().string()))
            continue;
        pendingFiles.push_back(p);
    }
    return pendingFiles;
}

}  // namespace

// Minimum delay between two attempts of the same pending build (seconds since
// the timestamp on the first line of its ".pending" file). force bypasses it.
static constexpr time_t PendingRetryBackoffSeconds = 60;

// Retries every deferred build. A retry is a full build attempt: the dependency
// names stored in ".pending" are for display only and are never matched against
// the index (they used to be, which left models in subfolders pending forever).
// Each builder validates its dependencies on disk first; one still missing
// refreshes ".pending" (BuildDepMissingException, logged at debug). Classified
// primary transient failures keep an existing marker and refresh its timestamp;
// permanent failures consume it. Build-lock contention leaves it untouched.
// Failures remain logged at warning level for the next bounded coordinator/sweep.
void buildPending(Database* db, const std::string& outputPath, bool force) {
    const fs::path buildDir = outputPath.empty() ? db->buildDirectory() : fs::path(outputPath);
    std::error_code ec;
    if (!fs::exists(buildDir, ec) || ec)
        return;

    const std::string outPath = buildDir.string();

    // Collect first: building creates and removes files in this directory, which
    // must not happen under a live directory iterator.
    const std::vector<fs::path> pendingFiles = collectPendingFiles(buildDir);

    for (const auto& pendingPath : pendingFiles) {
        const auto hash = pendingPath.stem().string();
        const PendingFileData pending = parsePendingFile(pendingPath);

        const time_t timeSinceLastAttempt = utils::currentUnixTimestamp() - pending.lastAttempt;
        if (timeSinceLastAttempt < PendingRetryBackoffSeconds && !force) {
            LOGD << "Skipping build attempt for hash " << hash
                 << " (last attempt " << timeSinceLastAttempt << " seconds ago)";
            continue;
        }

        std::vector<Entry> entries;
        {
            auto q = db->query(
                "SELECT path, hash, type, properties, mtime, size, depth FROM entries WHERE hash = "
                "?");
            q->bind(1, hash);
            while (q->fetch()) {
                entries.emplace_back(q->getText(0),
                                     q->getText(1),
                                     q->getInt(2),
                                     q->getText(3),
                                     q->getInt64(4),
                                     q->getInt64(5),
                                     q->getInt(6));
            }
        }

        if (entries.empty()) {
            // Entry no longer indexed: nothing to retry (cleanupBuild garbage collects it too)
            LOGD << "Removing pending file of unindexed hash " << hash;
            io::assureIsRemoved(pendingPath);
            continue;
        }

        for (const auto& e : entries) {
            try {
                LOGD << "Retrying pending build for " << e.path;
                buildInternal(db, e, outPath, force);
            } catch (const BuildDepMissingException& err) {
                LOGD << "Build of " << e.path << " is still pending: " << err.what();
            } catch (const BuildInProgressException& err) {
                LOGD << "Build of " << e.path << " is already in progress: " << err.what();
            } catch (const std::exception& err) {
                LOGW << "Pending build of " << e.path << " failed: " << err.what();
            }
        }
    }
}

// Same marker set as buildPending(): top-level "<hash>.pending" files only.
bool isBuildPending(Database* db) {
    return !collectPendingFiles(db->buildDirectory()).empty();
}

std::vector<PendingBuildInfo> getPendingBuildInfo(Database* db) {
    std::vector<PendingBuildInfo> result;

    for (const auto& pendingPath : collectPendingFiles(db->buildDirectory())) {
        const auto hash = pendingPath.stem().string();
        const PendingFileData pending = parsePendingFile(pendingPath);

        auto q = db->query("SELECT path FROM entries WHERE hash = ?");
        q->bind(1, hash);
        if (!q->fetch())
            continue;  // entry no longer indexed; garbage collected by cleanupBuild()

        PendingBuildInfo info;
        info.hash = hash;
        info.path = q->getText(0);
        info.missingDependencies = pending.missingDependencies;
        info.lastAttempt = pending.lastAttempt;

        result.push_back(std::move(info));
    }

    return result;
}


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

// Removes a leftover build folder and records it in the cleanup result.
static void removeOrphanFolder(const fs::path& p, CleanupResult& result) {
    std::error_code rmErr;
    fs::remove_all(p, rmErr);
    if (rmErr) {
        LOGW << "Failed to remove orphan temp folder '" << p.string()
             << "': " << rmErr.message();
    } else {
        result.removedBuilds.push_back(p.string());
        LOGD << "Removed orphan build temp folder: " << p.string();
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
            primaryLock.emplace(primaryDir.string(), false);
        } catch (const BuildInProgressException&) {
            LOGD << "Skipping 3D Tiles temp folders: a Model build is in progress in "
                 << hashDir.string();
        } catch (const AppException& e) {
            LOGW << "Cannot lock " << primaryDir.string() << " (" << e.what()
                 << "); skipping 3D Tiles temp folders";
        }
        if (primaryLock) {
            try {
                tilesLock.emplace(tilesDir.string(), false);
            } catch (const BuildInProgressException&) {
                LOGD << "Skipping 3D Tiles temp folders with active tiles build lock in: "
                     << hashDir.string();
            } catch (const AppException& e) {
                LOGW << "Cannot lock " << tilesDir.string() << " (" << e.what()
                     << "); skipping 3D Tiles temp folders";
            }
        }

        if (tilesLock) {
            const auto moveAside = [&](const fs::path& p) {
                const fs::path trash = hashDir / (tiles3DTrashPrefix() + utils::generateRandomString(16));
                std::error_code ec;
                fs::rename(p, trash, ec);
                if (ec) {
                    LOGW << "Cannot move orphan folder '" << p.string() << "' aside for deletion: "
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
                    LOGD << "Keeping " << oldFolder.string() << ": " << tilesDir.string()
                         << " has no complete tiles sentinel; a build can restore it";
            }
        }
        // Both locks are released here ("3dtiles" then "nxs"), before any deletion.
    }

    for (const auto& [trash, reported] : toDelete) {
        std::error_code rmErr;
        fs::remove_all(trash, rmErr);
        if (rmErr) {
            LOGW << "Failed to remove orphan temp folder '" << trash.string()
                 << "' (the next cleanup retries): " << rmErr.message();
        } else {
            result.removedBuilds.push_back(reported.string());
            LOGD << "Removed orphan build temp folder: " << reported.string();
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

        const std::string name = it->path().filename().string();
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
            if (name.rfind(tilesTrashPrefix, 0) == 0)
                tilesTrashFolders.push_back(it->path());
            else
                tilesTempFolders.push_back(it->path());
            continue;
        }

        const fs::path lockPath = hashDir / buildLockPath(subfolder);
        // A stale lock file is removed with the folder it guarded (Unix always did so).
        if (fs::exists(lockPath) && !BuildLock::removeIfStale((hashDir / subfolder).string())) {
            LOGD << "Skipping temp folder with active build lock: " << it->path().string();
            continue;
        }

        removeOrphanFolder(it->path(), result);
    }

    if (!tilesTempFolders.empty() || !tilesOldFolder.empty() || !tilesTrashFolders.empty())
        removeOrphanTilesFolders(hashDir, tilesTempFolders, tilesOldFolder, tilesTrashFolders, result);
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

    // Artifact names are validated with isHashLike() before deleting so we never
    // wipe unrelated files/directories that may live in a user-specified output path.

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
            if (validHashes.find(name) != validHashes.end()) {
                // Still referenced: keep the build output, but reclaim any
                // orphaned "<subfolder>-temp-<rand>" scratch folders left behind
                // by a build that crashed before its RAII cleanup ran (a SIGSEGV
                // bypasses the cleanup in buildInternal). Folders whose build is
                // still active (live .building lock) are skipped.
                removeOrphanTempFolders(p, result);
                continue;  // still referenced
            }

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
                    // "<subfolder>.building" is the lock of "<subfolder>"
                    if (subIt->path().extension() == BuildLockSuffix &&
                        BuildLock::isHeld((p / subIt->path().stem()).string())) {
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
        } else if (entry.is_regular_file(typeErr) && p.extension() == PendingMarkerExtension) {
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
        } else if (entry.is_regular_file(typeErr) && isPendingMarkerTempName(name)) {
            // Leftover of replacePendingMarker() interrupted between write and
            // rename. It is never valid state, whatever its hash, but a fresh one
            // may belong to a live writer: collect it only once it is old.
            std::error_code timeErr;
            const auto writeTime = fs::last_write_time(p, timeErr);
            if (timeErr) {
                LOGW << "Cannot read the age of pending marker temp file '" << p.string()
                     << "' (" << timeErr.message() << "); keeping it";
                continue;
            }
            if (fs::file_time_type::clock::now() - writeTime < PendingMarkerTempMinAge) {
                LOGD << "Keeping recent pending marker temp file: " << p.string();
                continue;
            }

            std::error_code rmErr;
            fs::remove(p, rmErr);
            if (rmErr) {
                LOGW << "Failed to remove pending marker temp file '" << p.string()
                     << "': " << rmErr.message();
            } else {
                result.removedBuilds.push_back(p.string());
                LOGD << "Removed stale pending marker temp file: " << p.string();
            }
        }
    }

    return result;
}

bool isBuildActive(Database* db, const std::string& path) {
    Entry e;
    const bool entryExists = getEntry(db, path, e);
    if (!entryExists)
        return false;

    std::string subfolder;
    if (!isBuildableInternal(e, subfolder)) {
        // For dependencies, use the main file's hash
        Entry mainEntry;
        if (!isBuildableDependency(e, [db](const std::string& p, Entry& out) { return getEntry(db, p, out); },
                                   mainEntry, subfolder))
            return false;

        e = mainEntry;
    }

    // Construct the output folder path
    std::string outPath = db->buildDirectory().string();
    fs::path baseOutputPath = fs::path(outPath) / e.hash;
    std::string outputFolder = (baseOutputPath / subfolder).string();
    std::string lockFile = buildLockPath(outputFolder);

    LOGD << "Checking for active build in: " << outputFolder;

    if (!fs::exists(lockFile)) {
        return false;  // No lock file means no active build
    }

    if (BuildLock::isHeld(outputFolder)) {
        LOGD << "Active build detected (kernel lock held): " << lockFile;
        return true;
    }

    // No live holder: the file is the leftover of a crashed build. It is removed here, as it
    // always was; a pure probe is BuildLock::isHeld. A build that took the lock since the probe
    // keeps it (removeIfStale fails) and is reported active.
    return !BuildLock::removeIfStale(outputFolder);
}

}  // namespace ddb
