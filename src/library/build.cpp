/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Build rules: which entries are buildable (directly or as the sidecar of a main
// file), where their artifacts go, and the status queries that read them
// (complete, active). Builds run in build_dispatch.cpp, pending markers live in
// pending_markers.cpp and the clean-up in build_cleanup.cpp.

#include "build.h"

#include <optional>

#include "3d.h"
#include "build_internal.h"
#include "build_layout.h"
#include "buildlock.h"
#include "dbops.h"
#include "exceptions.h"
#include "mio.h"

namespace ddb {

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

std::optional<BuildTarget> resolveBuildTarget(const Entry& e, const EntryLookup& lookup) {
    std::string subfolder;
    if (isBuildableInternal(e, subfolder))
        return BuildTarget{e, subfolder, false};

    Entry mainEntry;
    if (isBuildableDependency(e, lookup, mainEntry, subfolder))
        return BuildTarget{mainEntry, subfolder, true};

    return std::nullopt;
}

std::optional<BuildTarget> resolveBuildTarget(Database* db, const Entry& e) {
    return resolveBuildTarget(e, [db](const std::string& p, Entry& out) { return getEntry(db, p, out); });
}

bool isBuildable(Database* db, const std::string& path, std::string& subfolder) {
    Entry e;

    const bool entryExists = getEntry(db, path, e);
    if (!entryExists)
        throw InvalidArgsException(path + " is not a valid path in the database.");

    const auto target = resolveBuildTarget(db, e);
    if (!target)
        return false;

    subfolder = target->subfolder;
    return true;
}

// Returns true if the given file exists and has non-zero size.
bool fileExistsAndNonEmpty(const fs::path& p) {
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
bool isBuildOutputComplete(const fs::path& baseOutputPath,
                           const Entry& e,
                           const std::string& subfolder,
                           bool checkTilesSwap) {
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

// isBuildComplete() for a resolved target. For dependencies, completeness is
// defined by the main file's artifacts.
static bool isTargetBuildComplete(const fs::path& buildDirectory, const BuildTarget& target) {
    return isBuildOutputComplete(buildDirectory / target.entry.hash, target.entry, target.subfolder);
}

bool isBuildComplete(Database* db, const std::string& path) {
    Entry e;
    if (!getEntry(db, path, e))
        return false;

    const auto target = resolveBuildTarget(db, e);
    return target && isTargetBuildComplete(db->buildDirectory(), *target);
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

            const auto target = resolveBuildTarget(e, lookup);
            status.buildable = target.has_value();
            status.complete = target && isTargetBuildComplete(buildDirectory, *target);
        }

        result.push_back(std::move(status));
    }

    return result;
}

bool isBuildActive(Database* db, const std::string& path) {
    Entry e;
    const bool entryExists = getEntry(db, path, e);
    if (!entryExists)
        return false;

    // For dependencies, the lock is the main file's
    const auto target = resolveBuildTarget(db, e);
    if (!target)
        return false;

    // Construct the output folder path
    std::string outPath = db->buildDirectory().string();
    fs::path baseOutputPath = fs::path(outPath) / target->entry.hash;
    std::string outputFolder = (baseOutputPath / target->subfolder).string();
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
