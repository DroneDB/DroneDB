/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The build manifest (build_manifest.h): entry types, the state and the existing
// artifacts of entries, build file resolution and the copy of a build to another dataset. Every
// path comes from the EntryHandler registry and build_layout.h.

#include "build_manifest.h"
#include "json.h"
#include "randomutil.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <system_error>

#include "build_internal.h"
#include "build_layout.h"
#include "buildlock.h"
#include "database.h"
#include "deppath.h"
#include "entry_path.h"
#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "raster_cache.h"

namespace ddb {

// ---- JSON --------------------------------------------------------------------------------------

std::string_view buildStateName(BuildState state) {
    switch (state) {
        case BuildState::Pending: return "pending";
        case BuildState::Building: return "building";
        case BuildState::Complete: return "complete";
        case BuildState::None: break;
    }
    return "none";
}

namespace {

json stringOrNull(const std::string& s) {
    return s.empty() ? json(nullptr) : json(s);
}

// Integer Unix epoch seconds, or null
json unixSecondsOrNull(const std::optional<time_t>& t) {
    return t ? json(static_cast<long long>(*t)) : json(nullptr);
}

std::string_view thumbnailName(ThumbnailSource source) {
    switch (source) {
        case ThumbnailSource::File: return "file";
        case ThumbnailSource::BuildArtifact: return "build";
        case ThumbnailSource::None: break;
    }
    return "none";
}

}  // namespace

void to_json(json& j, const BuildArtifact& artifact) {
    j = {{"role", artifact.role},
              {"path", artifact.path},
              {"kind", artifact.kind == ArtifactKind::Directory ? "dir" : "file"},
              {"mediaType", artifact.mediaType}};
    if (!artifact.downloadName.empty())
        j["downloadName"] = artifact.downloadName;
    if (!artifact.index.empty())
        j["index"] = artifact.index;
    if (artifact.legacy)
        j["legacy"] = true;
}

void to_json(json& j, const EntryBuild& build) {
    json artifacts = json::array();
    for (const auto& a : build.artifacts)
        artifacts.push_back(a);
    j = {{"buildable", build.buildable},
         {"source", stringOrNull(build.source)},
         {"hash", stringOrNull(build.hash)},
         {"root", build.root.empty() ? json(nullptr) : json(io::toUtf8(build.root))},
         {"state", std::string(buildStateName(build.state))},
         {"missingDependencies", build.missingDependencies},
         {"lastAttempt", unixSecondsOrNull(build.lastAttempt)},
         {"retryAfter", unixSecondsOrNull(build.retryAfter)},
         {"artifacts", std::move(artifacts)}};
}

json entryTypesJson() {
    json types = json::array();
    for (const EntryHandler* handler : entryHandlers()) {
        json roles = json::array();
        for (const auto& a : handler->artifacts())
            roles.push_back(a.role);
        types.push_back({{"id", static_cast<int>(handler->type())},
                         {"name", handler->name()},
                         {"buildable", handler->buildable()},
                         {"thumbnail", std::string(thumbnailName(handler->thumbnailSource()))},
                         {"sidecarExtensions", handler->sidecarExtensions()},
                         {"roles", std::move(roles)}});
    }
    return types;
}

// ---- Filesystem probe --------------------------------------------------------------------------

std::optional<std::vector<DirectoryName>> BuildProbe::list(const fs::path& dir) const {
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec)
        return std::nullopt;
    std::vector<DirectoryName> names;
    for (const fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec)
            break;
        std::error_code typeError;
        const bool directory = it->is_directory(typeError) && !typeError;
        names.push_back({io::toUtf8(it->path().filename()), directory});
    }
    if (ec) {
        LOGD << "Cannot list " << io::toUtf8(dir) << " to the end: " << ec.message();
    }
    return names;
}

bool BuildProbe::isFile(const fs::path& path) const {
    std::error_code ec;
    return fs::is_regular_file(path, ec) && !ec;
}

std::optional<PendingMarker> BuildProbe::readPendingMarker(const fs::path& path) const {
    return ddb::readPendingMarker(path);
}

bool BuildProbe::isLockHeld(const fs::path& outputFolder) const {
    return BuildLock::isHeld(io::toUtf8(outputFolder));
}

bool BuildProbe::isComplete(const EntryHandler& handler, const fs::path& root) const {
    return handler.isComplete(root);
}

const BuildProbe& filesystemBuildProbe() {
    static const BuildProbe probe;
    return probe;
}

// ---- Existing artifacts ------------------------------------------------------------------------

namespace {

bool hasName(const std::vector<DirectoryName>& names, const std::string& name, bool directory) {
    return std::any_of(names.begin(), names.end(), [&](const DirectoryName& n) {
        return n.name == name && n.directory == directory;
    });
}

// Whether the artifacts of one build root exist, listing each folder at most once
class ArtifactLookup {
  public:
    ArtifactLookup(const fs::path& root, const std::vector<DirectoryName>& rootNames, const BuildProbe& probe)
        : root_(root), rootNames_(rootNames), probe_(probe) {}

    bool exists(const BuildArtifact& a) {
        // Every artifact folder is a directory of the build root: a missing one costs nothing
        const size_t firstSlash = a.path.find('/');
        if (firstSlash != std::string::npos && !hasName(rootNames_, a.path.substr(0, firstSlash), true))
            return false;

        // A directory exists when its index does: one query, never a listing of a folder that
        // can hold many files (tiles)
        if (a.kind == ArtifactKind::Directory && !a.index.empty())
            return hasName(rootNames_, a.path.substr(0, firstSlash), true) &&
                   probe_.isFile(root_ / io::fromUtf8(a.path) / io::fromUtf8(a.index));

        const size_t lastSlash = a.path.rfind('/');
        const std::string name = lastSlash == std::string::npos ? a.path : a.path.substr(lastSlash + 1);
        const bool directory = a.kind == ArtifactKind::Directory;
        if (lastSlash == std::string::npos)
            return hasName(rootNames_, name, directory);
        const auto* names = listing(a.path.substr(0, lastSlash));
        return names != nullptr && hasName(*names, name, directory);
    }

  private:
    const std::vector<DirectoryName>* listing(const std::string& folder) {
        auto it = listings_.find(folder);
        if (it == listings_.end())
            it = listings_.emplace(folder, probe_.list(root_ / io::fromUtf8(folder))).first;
        return it->second ? &*it->second : nullptr;
    }

    const fs::path& root_;
    const std::vector<DirectoryName>& rootNames_;
    const BuildProbe& probe_;
    std::map<std::string, std::optional<std::vector<DirectoryName>>> listings_;
};

}  // namespace

std::vector<BuildArtifact> existingArtifacts(const EntryHandler& handler, const fs::path& root,
                                             const std::vector<DirectoryName>& rootNames,
                                             const BuildProbe& probe) {
    ArtifactLookup lookup(root, rootNames, probe);
    std::vector<BuildArtifact> found;
    std::vector<BuildArtifact> legacy;
    for (auto& a : handler.artifacts()) {
        if (a.legacy)
            legacy.push_back(std::move(a));
        else if (lookup.exists(a))
            found.push_back(std::move(a));
    }
    // The fallback of a dataset built before the current layout (EPT before COPC): an older
    // output is served only while no current one exists
    if (found.empty())
        for (auto& a : legacy)
            if (lookup.exists(a))
                found.push_back(std::move(a));
    return found;
}

// ---- Build state -------------------------------------------------------------------------------

BuildStateReader::BuildStateReader(Database* db, const BuildProbe& probe)
    : targets_(db), buildDirectory_(fs::absolute(db->buildDirectory())), probe_(probe) {}

EntryBuild BuildStateReader::read(const Entry& e) {
    EntryBuild build;
    const auto target = targets_.resolve(e);
    if (!target)
        return build;  // not buildable: no filesystem query

    const EntryHandler& handler = *target->handler;
    build.buildable = true;
    build.source = target->entry.path;
    build.hash = target->entry.hash;
    build.root = buildDirectory_ / target->entry.hash;

    const auto rootNames = probe_.list(build.root);
    const auto marker = probe_.readPendingMarker(pendingMarkerPath(build.root));
    if (marker) {
        build.missingDependencies = marker->missingDependencies;
        build.lastAttempt = marker->lastAttempt;
        build.retryAfter = pendingRetryAfter(marker->lastAttempt);
    }
    // The lock of the whole build is the one of the primary output folder (buildInternal). Its
    // file exists while a build runs; a file without a live holder is a crashed build's leftover.
    const bool building = rootNames && hasName(*rootNames, buildLockPath(target->subfolder), false) &&
                          probe_.isLockHeld(build.root / target->subfolder);
    if (rootNames)
        build.artifacts = existingArtifacts(handler, build.root, *rootNames, probe_);

    if (building)
        build.state = BuildState::Building;
    else if (marker)
        build.state = BuildState::Pending;
    else if (rootNames && probe_.isComplete(handler, build.root))
        build.state = BuildState::Complete;
    return build;
}

json getBuildArtifactsJson(Database* db, const std::vector<std::string>& paths) {
    BuildStateReader reader(db);
    json result = json::array();
    for (const auto& path : paths) {
        const auto found = reader.lookup(path);
        if (!found) {
            result.push_back({{"path", path}, {"exists", false}});
            continue;
        }
        const Entry& e = *found;
        result.push_back({{"path", path},
                          {"exists", true},
                          {"hash", stringOrNull(e.hash)},
                          {"type", static_cast<int>(e.type)},
                          {"build", reader.read(e)}});
    }
    return result;
}

std::string listEntriesJson(Database* db, const EntryQuery& query, bool withBuild) {
    const auto entries = listEntries(db, query);
    std::ostringstream out;
    if (!withBuild) {
        writeEntries(out, entries, OutputFormat::Json);
        return out.str();
    }
    BuildStateReader reader(db);
    writeEntries(out, entries, OutputFormat::Json, [&reader](const Entry& e, json& j) { j["build"] = reader.read(e); });
    return out.str();
}

// ---- Build files -------------------------------------------------------------------------------

namespace {

void requireBuildHash(const std::string& hash) {
    if (!isHashLike(hash))
        throw InvalidArgsException("Invalid build hash '" + hash + "': expected 64 hexadecimal characters");
}

// The handlers of the types of the entries with this hash (usually one), by type value
std::vector<const EntryHandler*> handlersOfHash(Database* db, const std::string& hash) {
    const auto q = db->query("SELECT DISTINCT type FROM entries WHERE hash = ? ORDER BY type");
    q->bind(1, hash);
    std::vector<const EntryHandler*> handlers;
    while (q->fetch())
        handlers.push_back(&entryHandler(static_cast<EntryType>(q->getInt(0))));
    return handlers;
}

// path ('/'-separated, normalized) is a file artifact of handler or lies below one of its
// directory artifacts
bool isInsideArtifact(const EntryHandler& handler, const std::string& path) {
    const auto artifacts = handler.artifacts();
    return std::any_of(artifacts.begin(), artifacts.end(), [&](const BuildArtifact& a) {
        if (a.kind == ArtifactKind::File)
            return path == a.path;
        return path.size() > a.path.size() + 1 && path.starts_with(a.path) &&
               path[a.path.size()] == '/';
    });
}

}  // namespace

fs::path resolveBuildFile(Database* db, const std::string& hash, const std::string& relativePath) {
    requireBuildHash(hash);
    const EntryPathResult normalized = normalizeEntryPath(relativePath);
    if (!normalized.accepted())
        throw InvalidArgsException("Invalid build file path '" + relativePath + "' (" +
                                   std::string(normalized.rejected) + ")");

    const auto handlers = handlersOfHash(db, hash);
    if (handlers.empty())
        throw EntryNotFoundException("No entry has the build hash " + hash);
    if (std::none_of(handlers.begin(), handlers.end(),
                     [&](const EntryHandler* h) { return isInsideArtifact(*h, normalized.path); }))
        throw EntryNotFoundException("'" + normalized.path + "' is not a build artifact of " + hash);

    const fs::path root = fs::absolute(db->buildDirectory() / hash);
    const fs::path file = root / io::fromUtf8(normalized.path);
    std::error_code ec;
    if (!fs::is_regular_file(file, ec) || ec)
        throw EntryNotFoundException("Build file '" + normalized.path + "' of " + hash + " does not exist");

    // A link inside the build root must not lead out of it
    std::error_code fileError;
    std::error_code rootError;
    const fs::path realFile = fs::canonical(file, fileError);
    const fs::path realRoot = fs::canonical(root, rootError);
    if (fileError || rootError || !deppath::isInside(realFile, realRoot))
        throw EntryNotFoundException("Build file '" + normalized.path + "' of " + hash +
                                     " resolves outside its build folder");
    return file;
}

// ---- Copy --------------------------------------------------------------------------------------

namespace {

// Appends value to list unless it is there already
void appendUnique(std::vector<std::string>& list, const std::string& value) {
    if (std::find(list.begin(), list.end(), value) == list.end())
        list.push_back(value);
}

// Replaces the folder to with a copy of the folder from. The copy is made in a sibling scratch
// folder "<to>-temp-<rand16>" (removed on failure), then renamed onto to, so a reader never sees
// a half-copied folder under the artifact's name.
void copyFolderReplacing(const fs::path& from, const fs::path& to) {
    io::ScopedTempDir scratch(io::concat(to, TempFolderInfix + randomString(16)));
    std::error_code ec;
    fs::copy(from, scratch.path(), fs::copy_options::recursive, ec);
    if (ec)
        throw FSException("Cannot copy " + io::toUtf8(from) + " to " + io::toUtf8(scratch.path()) + " (" +
                              ec.message() + ")",
                          ec);
    if (fs::exists(to)) {
        // Rasters of the previous copy kept open would block the removal on Windows
        releaseCachedFiles(to);
        io::assureIsRemoved(to);
    }
    io::rename(scratch.path(), to);
    scratch.release();
}

}  // namespace

json copyBuildArtifacts(Database* source, Database* destination, const std::string& hash) {
    requireBuildHash(hash);
    if (io::isSamePath(source->buildDirectory(), destination->buildDirectory()))
        throw InvalidArgsException("The source and the destination of a build copy are the same dataset");

    std::vector<const EntryHandler*> handlers;
    for (const EntryHandler* h : handlersOfHash(source, hash))
        if (h->buildable())
            handlers.push_back(h);
    if (handlers.empty())
        throw EntryNotFoundException("No buildable entry has the build hash " + hash + " in the source dataset");

    // The folders whose locks exclude a build (the primary output first: the lock order of a
    // build), and every folder an artifact lives in, legacy ones included
    std::vector<std::string> lockFolders;
    std::vector<std::string> copyFolders;
    for (const EntryHandler* h : handlers) {
        appendUnique(lockFolders, h->subfolder());
        for (const auto& folder : h->artifactFolders())
            appendUnique(lockFolders, folder);
        for (const auto& a : h->artifacts())
            appendUnique(copyFolders, a.path.substr(0, a.path.find('/')));
    }

    const fs::path sourceRoot = fs::absolute(source->buildDirectory() / hash);
    const fs::path destinationRoot = fs::absolute(destination->buildDirectory() / hash);

    // A build running in the source may be publishing its output: copy nothing
    for (const auto& folder : lockFolders)
        if (BuildLock::isHeld(io::toUtf8(sourceRoot / folder)))
            throw BuildInProgressException("A build of " + hash + " runs in the source dataset");

    try {
        io::assureFolderExists(destinationRoot);
        // No build of the destination can start while the copy replaces its folders
        std::vector<BuildLock> locks;
        locks.reserve(lockFolders.size());
        for (const auto& folder : lockFolders)
            locks.emplace_back(io::toUtf8(destinationRoot / folder));

        ensureSufficientDiskSpace(destinationRoot);
        for (const auto& folder : copyFolders) {
            std::error_code ec;
            if (fs::is_directory(sourceRoot / folder, ec) && !ec)
                copyFolderReplacing(sourceRoot / folder, destinationRoot / folder);
        }
        const fs::path sourceMarker = pendingMarkerPath(sourceRoot);
        std::error_code ec;
        if (fs::is_regular_file(sourceMarker, ec) && !ec)
            copyPendingMarker(sourceMarker, pendingMarkerPath(destinationRoot));
    } catch (const FSException& ex) {
        if (ex.code() == std::errc::no_space_on_device)
            throw RetryableBuildException(ex.what(), RetryableBuildReason::NoSpaceOnDevice);
        throw;
    }

    // What the destination has now
    const BuildProbe& probe = filesystemBuildProbe();
    json artifacts = json::array();
    std::vector<std::string> seen;
    if (const auto rootNames = probe.list(destinationRoot)) {
        for (const EntryHandler* h : handlers)
            for (const auto& a : existingArtifacts(*h, destinationRoot, *rootNames, probe)) {
                const std::string key = a.role + "\n" + a.path;
                if (std::find(seen.begin(), seen.end(), key) != seen.end())
                    continue;
                seen.push_back(key);
                artifacts.push_back(a);
            }
    }
    return {{"hash", hash},
            {"artifacts", std::move(artifacts)},
            {"pending", probe.isFile(pendingMarkerPath(destinationRoot))}};
}

}  // namespace ddb
