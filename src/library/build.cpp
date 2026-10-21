/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Build rules: which entries are buildable (directly or as the sidecar of a main
// file), where their artifacts go, and the status queries that read them
// (complete, active). Builds run in build_dispatch.cpp, pending markers live in
// pending_markers.cpp and the clean-up in build_cleanup.cpp.

#include "build.h"

#include <optional>

#include "build_internal.h"
#include "build_layout.h"
#include "buildlock.h"
#include "dbops.h"
#include "entry_handler.h"
#include "exceptions.h"
#include "mio.h"
#include "utils.h"

namespace ddb {

// Entry paths are dataset-relative, so the folder is kept. Until the main file is
// looked up case-insensitively, the candidates are the lowercase and uppercase spellings
// of each main extension.
std::vector<std::string> sidecarMainCandidates(const std::string& path) {
    std::vector<std::string> candidates;
    for (const auto& ext : sidecarMainExtensions(path)) {
        std::string upper = ext;
        utils::toUpper(upper);
        for (const auto& spelling : {ext, upper})
            candidates.push_back(fs::path(path).replace_extension("." + spelling).generic_string());
    }
    return candidates;
}

static std::vector<std::string> dependencyMainCandidates(const Entry& e) {
    if (e.type != EntryType::Generic)
        return {};
    return sidecarMainCandidates(e.path);
}

// The handler that builds e, or nullptr when e is not built by itself
static const EntryHandler* buildingHandler(const Entry& e) {
    const EntryHandler& handler = entryHandler(e.type);
    return handler.isBuildable(e) ? &handler : nullptr;
}

// Special case: some files are not buildable by themselves but trigger the
// (forced) rebuild of their main file, e.g. the .dbf of a shapefile or the .prj
// of a drawing (SidecarRule). A sidecar is buildable only while its main file is
// indexed: an orphan .prj next to a raster is not (otherwise it is never "complete"
// and is rebuilt forever by completeness sweeps).
static std::optional<BuildTarget> resolveDependencyTarget(const Entry& e, const EntryLookup& lookup) {
    for (const auto& candidate : dependencyMainCandidates(e)) {
        Entry mainEntry;
        if (!lookup(candidate, mainEntry))
            continue;
        if (const EntryHandler* handler = buildingHandler(mainEntry))
            return BuildTarget{mainEntry, handler->subfolder(), handler, true};
    }
    return std::nullopt;
}

std::optional<BuildTarget> resolveBuildTarget(const Entry& e, const EntryLookup& lookup) {
    if (const EntryHandler* handler = buildingHandler(e))
        return BuildTarget{e, handler->subfolder(), handler, false};

    return resolveDependencyTarget(e, lookup);
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

// isBuildComplete() for a resolved target. For dependencies, completeness is
// defined by the main file's artifacts.
static bool isTargetBuildComplete(const fs::path& buildDirectory, const BuildTarget& target) {
    return target.handler->isComplete(buildDirectory / target.entry.hash);
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
