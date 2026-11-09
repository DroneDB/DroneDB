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
#include "fileversion.h"
#include "hash.h"
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
    if (!artifact.fallbackFor.empty())
        j["fallbackFor"] = artifact.fallbackFor;
    j["version"] = artifact.version;
}

void to_json(json& j, const EntryBuild& build) {
    json artifacts = json::array();
    for (const auto& a : build.artifacts)
        artifacts.push_back(a);
    j = {{"buildable", build.buildable},
         {"source", stringOrNull(build.source)},
         {"hash", stringOrNull(build.hash)},
         {"key", stringOrNull(build.key)},
         {"root", build.root.empty() ? json(nullptr) : json(io::toUtf8(build.root))},
         {"state", std::string(buildStateName(build.state))},
         {"missingDependencies", build.missingDependencies},
         {"awaitingFiles", build.awaitingFiles},
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

std::string BuildProbe::version(const fs::path& file) const {
    return buildFileVersion(file);
}

std::string buildFileVersion(const fs::path& file) {
    const std::string version = fileVersion(file);
    return version.empty() ? std::string() : Hash::strCRC64(version);
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
    auto artifacts = handler.artifacts();
    // The roles of the current artifacts that exist
    std::vector<std::string> current;
    for (const auto& a : artifacts)
        if (!a.legacy && lookup.exists(a))
            current.push_back(a.role);
    const auto has = [&current](const std::string& role) {
        return std::find(current.begin(), current.end(), role) != current.end();
    };
    // The fallback of a dataset built before the current layout (EPT before COPC): an older
    // output is served only while the role it stands in for is missing, whatever else exists
    std::vector<BuildArtifact> found;
    for (auto& a : artifacts)
        if (a.legacy ? !has(a.fallbackFor) && lookup.exists(a) : has(a.role)) {
            a.version = probe.version(root / io::fromUtf8(a.openedPath()));
            found.push_back(std::move(a));
        }
    return found;
}

void removeSupersededArtifacts(const EntryHandler& handler,
                               const fs::path& root,
                               const BuildProbe& probe) {
    const auto artifacts = handler.artifacts();
    if (std::none_of(artifacts.begin(), artifacts.end(), [](const BuildArtifact& a) {
            return a.legacy;
        }))
        return;
    const auto rootNames = probe.list(root);
    if (!rootNames)
        return;
    const auto existing = existingArtifacts(handler, root, *rootNames, probe);
    for (const auto& legacy : artifacts) {
        if (!legacy.legacy)
            continue;
        const bool replaced =
            std::any_of(existing.begin(), existing.end(), [&](const BuildArtifact& a) {
                return !a.legacy && a.role == legacy.fallbackFor;
            });
        if (!replaced)
            continue;
        // Cleanup after a published build: a folder that cannot go now (a file kept open on
        // Windows, a permission) must not fail that build. The next build of the entry tries
        // again, also when it finds the output complete.
        // The folder of the legacy output goes whole, unless a current artifact lives in it too
        // (a splat's model.spz next to its model.rad): then only the legacy output itself goes
        const bool sharesFolder =
            std::any_of(artifacts.begin(), artifacts.end(), [&](const BuildArtifact& a) {
                return !a.legacy && a.folder() == legacy.folder();
            });
        const fs::path target = root / io::fromUtf8(sharesFolder ? legacy.path : legacy.folder());
        try {
            io::assureIsRemoved(target);
        } catch (const std::exception& e) {
            LOGW << "Cannot remove the superseded build output '" << io::toUtf8(target)
                 << "': " << e.what();
        }
    }
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
    build.key = target->key;
    build.root = buildDirectory_ / io::fromUtf8(target->key);

    const auto rootNames = probe_.list(build.root);
    // The marker of this entry's build: the one of its path (a twin of the same content has its own)
    const auto marker =
        probe_.readPendingMarker(pendingMarkerPath(buildDirectory_, target->key, target->entry.path));
    if (marker) {
        build.missingDependencies = marker->missingDependencies;
        build.awaitingFiles = marker->awaitsFiles();
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

void requireBuildKey(const std::string& key) {
    if (!isBuildKey(key))
        throw InvalidArgsException("Invalid build hash '" + key +
                                   "': expected a build key (64 hexadecimal characters, then optionally '-' and " +
                                   std::to_string(GeorefDigestLength) + " more)");
}

// The handlers of the types of the entries whose build has this key (usually one), by type value
std::vector<const EntryHandler*> handlersOfKey(Database* db, const std::string& key) {
    const auto q = db->query(
        "SELECT type, json_extract(properties, '$.georef') FROM entries WHERE hash = ? ORDER BY type");
    q->bind(1, std::string(buildKeyHash(key)));
    std::vector<const EntryHandler*> handlers;
    const std::string hash(buildKeyHash(key));
    while (q->fetch()) {
        const auto type = static_cast<EntryType>(q->getInt(0));
        if (buildKeyOfRow(hash, type, q->getText(1)) != key)
            continue;
        const EntryHandler& handler = entryHandler(type);
        if (std::find(handlers.begin(), handlers.end(), &handler) == handlers.end())
            handlers.push_back(&handler);
    }
    return handlers;
}

// The artifact of handler that path ('/'-separated, normalized) lies in: the file artifact it
// names, or the directory artifact it is below; nothing otherwise
std::optional<BuildArtifact> artifactContaining(const EntryHandler& handler, const std::string& path) {
    for (auto& a : handler.artifacts()) {
        const bool inside = a.kind == ArtifactKind::File
                                ? path == a.path
                                : path.size() > a.path.size() + 1 && path.starts_with(a.path) &&
                                      path[a.path.size()] == '/';
        if (inside)
            return std::move(a);
    }
    return std::nullopt;
}

}  // namespace

json ResolvedBuildFile::toJson() const {
    json j = {{"path", io::toUtf8(path)}};
    if (!mediaType.empty())
        j["mediaType"] = mediaType;
    if (!version.empty())
        j["version"] = version;
    return j;
}

ResolvedBuildFile resolveBuildFile(Database* db, const std::string& hash, const std::string& relativePath) {
    // The name of the build is its key: the hash of the entries that name a place-independent build
    requireBuildKey(hash);
    const EntryPathResult normalized = normalizeEntryPath(relativePath);
    if (!normalized.accepted())
        throw InvalidArgsException("Invalid build file path '" + relativePath + "' (" +
                                   std::string(normalized.rejected) + ")");

    const auto handlers = handlersOfKey(db, hash);
    if (handlers.empty())
        throw EntryNotFoundException("No entry has the build hash " + hash);
    // The artifact of the first handler that names the file as the one a client opens, else of
    // the first that holds it inside a directory artifact
    std::optional<BuildArtifact> artifact;
    for (const EntryHandler* handler : handlers) {
        auto found = artifactContaining(*handler, normalized.path);
        if (!found)
            continue;
        const bool opens = normalized.path == found->openedPath();
        if (!artifact || opens)
            artifact = std::move(found);
        if (opens)
            break;
    }
    if (!artifact)
        throw EntryNotFoundException("'" + normalized.path + "' is not a build artifact of " + hash);

    const fs::path root = fs::absolute(db->buildDirectory() / io::fromUtf8(hash));
    // The request names the file with '/' (the separator of build URLs and of the manifest): the
    // host serves the returned path as it is, so it is the native form, '\\' on Windows
    const fs::path file = (root / io::fromUtf8(normalized.path)).make_preferred();
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
    // The version the build object lists for the artifact: what a URL of any of its files names
    const bool opened = normalized.path == artifact->openedPath();
    return {file, opened ? artifact->mediaType : std::string(),
            buildFileVersion((root / io::fromUtf8(artifact->openedPath())).make_preferred())};
}

// ---- Copy --------------------------------------------------------------------------------------

namespace {

// Appends value to list unless it is there already
void appendUnique(std::vector<std::string>& list, const std::string& value) {
    if (std::find(list.begin(), list.end(), value) == list.end())
        list.push_back(value);
}

// The pending markers (build_layout.h) of the build with this key that are in a build directory
std::vector<fs::path> pendingMarkersOfKey(const fs::path& buildDirectory, const std::string& key) {
    std::vector<fs::path> markers;
    std::error_code ec;
    fs::directory_iterator it(buildDirectory, ec);
    for (const fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
        const auto name = parsePendingMarkerName(io::toUtf8(it->path().filename()));
        if (name && !name->legacy() && name->key == key)
            markers.push_back(it->path());
    }
    return markers;
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
    requireBuildKey(hash);
    if (io::isSamePath(source->buildDirectory(), destination->buildDirectory()))
        throw InvalidArgsException("The source and the destination of a build copy are the same dataset");

    std::vector<const EntryHandler*> handlers;
    for (const EntryHandler* h : handlersOfKey(source, hash))
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
        for (const auto& folder : h->artifactFolders(true))
            appendUnique(copyFolders, folder);
    }

    const fs::path sourceRoot = fs::absolute(source->buildDirectory() / io::fromUtf8(hash));
    const fs::path destinationRoot = fs::absolute(destination->buildDirectory() / io::fromUtf8(hash));

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
        // The markers of the entries whose build this is, one per path
        for (const auto& sourceMarker : pendingMarkersOfKey(source->buildDirectory(), hash)) {
            const fs::path destinationMarker = destination->buildDirectory() / sourceMarker.filename();
            copyPendingMarker(sourceMarker, destinationMarker);
            // A transfer indexes its files in the destination before it copies the builds: the
            // add of a file the marker awaits came first and released nothing. Checked after the
            // copy, as a build attempt checks after its write, so a later add sees the marker
            releasePendingMarkerIfIndexed(destination, destinationMarker);
        }
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
    const auto destinationMarkers = pendingMarkersOfKey(destination->buildDirectory(), hash);
    return {{"hash", std::string(buildKeyHash(hash))},
            {"key", hash},
            {"artifacts", std::move(artifacts)},
            {"pending", std::any_of(destinationMarkers.begin(), destinationMarkers.end(),
                                    [&](const fs::path& m) { return probe.isFile(m); })}};
}

}  // namespace ddb
