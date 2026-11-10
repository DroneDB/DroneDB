/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Pending markers: "<key>.<pathId>.pending" records the build of one entry deferred until its
// missing dependencies are uploaded (build_layout.h: one marker per entry path, so two entries with
// the same content each wait for their own files). Writing (atomically), parsing and listing
// markers, releasing those that await an added file, retrying the deferred builds, and moving the
// markers of the layout before the build key ("<hash>.pending") to it.

#include "build.h"
#include "timeutil.h"

#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <unordered_set>

#include "build_internal.h"
#include "build_layout.h"
#include "cancellation.h"
#include "deppath.h"
#include "dbops.h"
#include "exceptions.h"
#include "index_sql.h"
#include "mio.h"
#include "stringutil.h"

namespace ddb {

namespace {

// A reader sees the old marker or the new one, never a part; the previous marker is kept on any
// failure (especially ENOSPC: a full disk must never remove the last pending marker). The sibling
// is "<marker>.tmp-<rand16>" (see pendingMarkerTempInfix()). Throws FSException.
void writeMarkerAtomically(const fs::path& pendingPath, const std::string& content) {
    io::writeFileAtomic(pendingPath, content, "pending marker", false, PendingMarkerTempTag);
}

}  // namespace

// Write the complete marker into a sibling file before replacing the old one.
// Failure (especially ENOSPC) must never remove the last pending marker.
bool replacePendingMarker(const fs::path& pendingPath, const std::string& dependencyLines) {
    try {
        writeMarkerAtomically(pendingPath, std::to_string(currentUnixTimestamp()) + "\n" + dependencyLines);
        return true;
    } catch (const std::exception& ex) {
        LOGW << "Cannot refresh pending marker '" << io::toUtf8(pendingPath)
             << "'; original marker retained: " << ex.what();
        return false;
    }
}

bool releasePendingMarker(const fs::path& pendingPath) {
    try {
        writeMarkerAtomically(pendingPath, "0\n");
        return true;
    } catch (const std::exception& ex) {
        LOGW << "Cannot release pending marker '" << io::toUtf8(pendingPath)
             << "'; original marker retained: " << ex.what();
        return false;
    }
}

bool pendingDependencyExists(const std::vector<std::string>& names, const PendingDependencyLookup& where) {
    const auto isFile = [](const fs::path& p) {
        std::error_code ec;
        return fs::is_regular_file(p, ec) && !ec;
    };
    try {
        for (const auto& name : names) {
            const fs::path rel = io::fromUtf8(name);
            if (isFile(where.datasetRoot / rel))
                return true;
            for (const auto& source : where.sourcePaths)
                if (isFile(where.datasetRoot / io::fromUtf8(source).parent_path() / rel))
                    return true;
        }
        for (const auto& p : where.extraPaths)
            if (isFile(io::fromUtf8(p)))
                return true;
    } catch (const std::exception& ex) {
        LOGD << "Cannot look for a pending dependency: " << ex.what();
    }
    return false;
}

bool writePendingMarker(const fs::path& pendingPath, const std::vector<std::string>& dependencies,
                        const PendingDependencyLookup& where) {
    std::string lines;
    for (const auto& d : dependencies)
        lines += d + "\n";
    if (!replacePendingMarker(pendingPath, lines))
        return false;
    // After the write, never before it (see the declaration)
    releasePendingMarkerIfSatisfied(pendingPath, dependencies, where);
    return true;
}

void releasePendingMarkerIfSatisfied(const fs::path& pendingPath, const std::vector<std::string>& dependencies,
                                     const PendingDependencyLookup& where) {
    if (dependencies.empty() || !pendingDependencyExists(dependencies, where))
        return;
    LOGD << "A dependency of " << io::toUtf8(pendingPath) << " exists now, releasing it";
    releasePendingMarker(pendingPath);
}

void copyPendingMarker(const fs::path& from, const fs::path& to) {
    std::ifstream in(from, std::ios::binary);
    if (!in.is_open())
        throw FSException("Cannot read pending marker " + io::toUtf8(from));
    const std::string content(std::istreambuf_iterator<char>(in), {});
    if (in.bad())
        throw FSException("Cannot read pending marker " + io::toUtf8(from));
    writeMarkerAtomically(to, content);
}

std::optional<PendingMarker> readPendingMarker(const fs::path& pendingFilePath) {
    std::ifstream pendingFile(pendingFilePath);
    if (!pendingFile)
        return std::nullopt;

    PendingMarker data;
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
        const time_t currentTime = currentUnixTimestamp();
        if (data.lastAttempt > currentTime) {
            LOGW << "Timestamp in pending file is in the future: " << line;
        }
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

bool isPendingMarkerTempName(const std::string& name) {
    const std::string infix = pendingMarkerTempInfix();
    const size_t infixAt = name.find(infix);
    if (infixAt == std::string::npos || name.size() <= infixAt + infix.size())
        return false;
    // The marker the sibling replaces: its name is what is before the infix, with the extension
    return parsePendingMarkerName(name.substr(0, infixAt) + PendingMarkerExtension).has_value();
}

namespace {

// Format of a ".pending" marker file (its name: build_layout.h), created by buildInternal() when a build is deferred due
// to missing dependencies (see its BuildDepMissingException handler in build_dispatch.cpp):
// the first line is a Unix timestamp of the last attempt, the following lines are missing
// dependency names (one per line). Names are dataset-root-relative paths of companion files
// (e.g. "sub/m.mtl", "sub/mats/tex.png"); markers written by older versions may hold names
// relative to the source file's folder. A marker that lists dependencies awaits files: no timer
// retries it, adding a file of the same name releases it (the released form is "0" and no
// dependency). A retry never matches the names against the index.

// A marker that cannot be read counts as one with no timestamp and no dependency (the build is
// due immediately), the historical, lenient behavior of buildPending().
PendingMarker parsePendingFile(const fs::path& pendingFilePath) {
    return readPendingMarker(pendingFilePath).value_or(PendingMarker{});
}

// Collects the ".pending" markers of a build directory: only top-level regular files with the name
// of a marker (parsePendingMarkerName: buildInternal writes them next to the per-key output
// folders), never nested or other names. Collecting first also keeps
// callers that build from mutating the directory under a live iterator.
// Iteration errors are logged and end the scan (never thrown).
std::vector<fs::path> collectPendingFiles(const fs::path& buildDir) {
    std::vector<fs::path> pendingFiles;
    std::error_code ec;
    if (!fs::exists(buildDir, ec) || ec)
        return pendingFiles;

    fs::directory_iterator it(buildDir, ec);
    if (ec) {
        LOGW << "Cannot open build directory '" << io::toUtf8(buildDir) << "': " << ec.message();
        return pendingFiles;
    }
    for (const fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) {
            LOGW << "Error iterating build directory '" << io::toUtf8(buildDir) << "': " << ec.message();
            break;
        }
        const fs::path& p = it->path();
        std::error_code typeErr;
        if (p.extension() != PendingMarkerExtension || !it->is_regular_file(typeErr) || typeErr)
            continue;
        if (!parsePendingMarkerName(io::toUtf8(p.filename())))
            continue;
        pendingFiles.push_back(p);
    }
    return pendingFiles;
}

// The entries of db with this hash (all their properties: the build key reads the georeference)
std::vector<Entry> entriesOfHash(Database* db, const std::string& hash) {
    std::vector<Entry> entries;
    auto q = db->query(selectEntries(EntryColumns::Properties) + " WHERE e.hash = ?");
    q->bind(1, hash);
    while (q->fetch()) {
        Entry e;
        readEntry(*q, EntryColumns::Properties, e);
        entries.push_back(std::move(e));
    }
    return entries;
}

// The entries the marker at pendingPath is the marker of: the entries of its hash that have its
// name (build_layout.h pendingMarkerName). A legacy marker "<hash>.pending" is the marker of every
// entry of that hash.
std::vector<Entry> entriesOfMarker(Database* db, const fs::path& pendingPath, const PendingMarkerName& name) {
    std::vector<Entry> entries = entriesOfHash(db, std::string(buildKeyHash(name.key)));
    if (name.legacy())
        return entries;
    const std::string fileName = io::toUtf8(pendingPath.filename());
    std::erase_if(entries, [&](const Entry& e) {
        return pendingMarkerName(entryHandler(e.type).buildKey(e), e.path) != fileName;
    });
    return entries;
}

// An added file (its lowercased dataset-relative path) is the file a marker of an entry awaits under the name
// dep: the name is dataset-relative (what a build writes), or relative to the folder of the entry (the names of a
// marker of an older version)
bool anyOwnerAwaits(const std::vector<Entry>& owners, const std::string& dep,
                    const std::unordered_set<std::string>& addedPaths) {
    const auto added = [&](const std::string& path) {
        return addedPaths.count(str::toLowerCopy(deppath::normalize(path))) != 0;
    };
    if (added(dep))
        return true;
    for (const Entry& e : owners) {
        const size_t slash = e.path.find_last_of('/');
        if (slash != std::string::npos && added(e.path.substr(0, slash + 1) + dep))
            return true;
    }
    return false;
}

}  // namespace

void migrateLegacyPendingMarkers(Database* db, const fs::path& buildRoot) {
    for (const auto& markerPath : collectPendingFiles(buildRoot)) {
        const auto name = parsePendingMarkerName(io::toUtf8(markerPath.filename()));
        if (name && name->legacy())
            migrateLegacyPendingMarker(db, buildRoot, name->key);
    }
}

void migrateLegacyPendingMarker(Database* db, const fs::path& buildRoot, const std::string& hash) {
    const fs::path legacy = legacyPendingMarkerPath(buildRoot, hash);
    std::error_code ec;
    if (!fs::is_regular_file(legacy, ec) || ec)
        return;
    try {
        for (const Entry& e : entriesOfHash(db, hash)) {
            // A marker of an entry that no build handles (a sidecar) is of no build
            const EntryHandler& handler = entryHandler(e.type);
            if (!handler.buildable())
                continue;
            const fs::path target = pendingMarkerPath(buildRoot, handler.buildKey(e), e.path);
            // The marker the entry has already is newer than the one of the old layout. The names the
            // legacy marker holds are those of the last entry of the content that was built (dataset-relative,
            // "mixA/brighton.mtl"): copied to a twin they would await a file of another folder forever. Each
            // entry gets the released form instead, so the next retry looks for its own files on disk and
            // writes the names of its own path
            if (!fs::exists(target, ec))
                writeMarkerAtomically(target, "0\n");
        }
        // Once every copy is written: a failure above keeps the legacy marker for the next call
        io::assureIsRemoved(legacy);
        LOGD << "Moved the pending marker " << io::toUtf8(legacy.filename()) << " to per-path markers";
    } catch (const std::exception& ex) {
        LOGW << "Cannot move the pending marker '" << io::toUtf8(legacy) << "' to per-path markers: " << ex.what();
    }
}

// Retries every deferred build whose retry is due (pendingRetryAfter(), build_internal.h) and that
// awaits no file (PendingMarker::awaitsFiles(): an add of one of its files releases it, see
// releasePendingBuildsAwaiting()); force retries them all. A retry is a full build attempt: the
// dependency names stored in ".pending" are never matched against the index (they used to be,
// which left models in subfolders pending forever); the release matches file names only.
// Each builder validates its dependencies on disk first; one still missing
// refreshes ".pending" (BuildDepMissingException, logged at debug). Classified
// primary transient failures keep an existing marker and refresh its timestamp;
// permanent failures consume it. Build-lock contention leaves it untouched.
// Failures remain logged at warning level for the next bounded coordinator/sweep.
void buildPending(Database* db, const std::string& outputPath, bool force, const BuildOptions& options) {
    const fs::path buildDir = outputPath.empty() ? db->buildDirectory() : io::fromUtf8(outputPath);
    std::error_code ec;
    if (!fs::exists(buildDir, ec) || ec) {
        options.reportProgress(1.0);
        return;
    }

    const std::string outPath = io::toUtf8(buildDir);

    // The markers of the layout before the build key become the markers of their entries
    migrateLegacyPendingMarkers(db, buildDir);

    // Collect first: building creates and removes files in this directory, which
    // must not happen under a live directory iterator.
    const std::vector<fs::path> pendingFiles = collectPendingFiles(buildDir);

    // The progress of the call is the fraction of the markers done plus the fraction of the one in the build
    const double markers = static_cast<double>(pendingFiles.size());
    size_t markerIndex = 0;
    for (const auto& pendingPath : pendingFiles) {
        const double begin = static_cast<double>(markerIndex) / markers;
        const double end = static_cast<double>(markerIndex + 1) / markers;
        markerIndex++;
        const auto markerName = io::toUtf8(pendingPath.filename());
        const auto name = parsePendingMarkerName(markerName);
        if (!name) {
            options.reportProgress(end);
            continue;
        }
        const PendingMarker pending = parsePendingFile(pendingPath);

        if (!force && pending.awaitsFiles()) {
            LOGD << "Skipping build attempt for " << markerName << " (awaits "
                 << pending.missingDependencies.size() << " missing file(s))";
            options.reportProgress(end);
            continue;
        }

        const time_t now = currentUnixTimestamp();
        const time_t retryAfter = pendingRetryAfter(pending.lastAttempt);
        if (!force && now < retryAfter) {
            LOGD << "Skipping build attempt for " << markerName << " (last attempt "
                 << now - pending.lastAttempt << " seconds ago, retry after " << retryAfter << ")";
            options.reportProgress(end);
            continue;
        }

        const std::vector<Entry> entries = entriesOfMarker(db, pendingPath, *name);

        if (entries.empty()) {
            // Entry no longer indexed: nothing to retry (cleanupBuild garbage collects it too)
            LOGD << "Removing pending file " << markerName << " of an entry that is not indexed";
            io::assureIsRemoved(pendingPath);
            options.reportProgress(end);
            continue;
        }

        size_t entryIndex = 0;
        for (const auto& e : entries) {
            // A canceled call stops before the next build
            cancel::throwIfCanceled();
            const double entryBegin = begin + (end - begin) * static_cast<double>(entryIndex) / static_cast<double>(entries.size());
            entryIndex++;
            const double entryEnd = begin + (end - begin) * static_cast<double>(entryIndex) / static_cast<double>(entries.size());
            try {
                LOGD << "Retrying pending build for " << e.path;
                buildInternal(db, e, outPath, force, options.forSlice(entryBegin, entryEnd));
            } catch (const CanceledException&) {
                throw;
            } catch (const BuildDepMissingException& err) {
                LOGD << "Build of " << e.path << " is still pending: " << err.what();
            } catch (const BuildInProgressException& err) {
                LOGD << "Build of " << e.path << " is already in progress: " << err.what();
            } catch (const std::exception& err) {
                LOGW << "Pending build of " << e.path << " failed: " << err.what();
            }
            options.reportProgress(entryEnd);
        }
    }
    options.reportProgress(1.0);
}

// Same marker set as buildPending(): top-level marker files only, and only those a
// retry can progress (a marker that awaits files waits for an add, not for a retry).
bool isBuildPending(Database* db) {
    for (const auto& pendingPath : collectPendingFiles(db->buildDirectory()))
        if (!parsePendingFile(pendingPath).awaitsFiles())
            return true;
    return false;
}

namespace {

// The file name of a dataset-relative path, lowercased (ASCII): the key a dependency name of a
// marker and an added file match by
std::string fileNameKey(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return str::toLowerCopy(slash == std::string::npos ? path : path.substr(slash + 1));
}

}  // namespace

void releasePendingMarkerIfIndexed(Database* db, const fs::path& pendingPath) {
    try {
        const auto pending = readPendingMarker(pendingPath);
        if (!pending || !pending->awaitsFiles())
            return;
        // lower() of SQLite folds ASCII only, as fileNameKey() does
        const auto q = db->query("SELECT path FROM entries WHERE " +
                                 typeCondition("type", "!=", EntryType::Directory) +
                                 " AND (lower(path) = ?1 OR substr(lower(path), -length(?1) - 1) = '/' || ?1) LIMIT 1");
        for (const auto& dep : pending->missingDependencies) {
            q->bind(1, fileNameKey(dep));
            const bool indexed = q->fetch();
            q->reset();
            if (!indexed)
                continue;
            LOGD << "Releasing pending build " << io::toUtf8(pendingPath.stem()) << ": " << dep
                 << " is indexed already";
            releasePendingMarker(pendingPath);
            return;
        }
    } catch (const std::exception& ex) {
        LOGW << "Cannot check the pending marker '" << io::toUtf8(pendingPath) << "' against the index: "
             << ex.what();
    }
}

void releasePendingBuildsAwaiting(Database* db, const std::vector<std::string>& filePaths) {
    if (filePaths.empty())
        return;
    try {
        const std::vector<fs::path> pendingFiles = collectPendingFiles(db->buildDirectory());
        if (pendingFiles.empty())
            return;

        std::unordered_set<std::string> addedNames, addedPaths;
        for (const auto& p : filePaths) {
            addedNames.insert(fileNameKey(p));
            addedPaths.insert(str::toLowerCopy(p));
        }

        for (const auto& pendingPath : pendingFiles) {
            const auto name = parsePendingMarkerName(io::toUtf8(pendingPath.filename()));
            if (!name)
                continue;
            const auto pending = readPendingMarker(pendingPath);
            if (!pending || !pending->awaitsFiles())
                continue;
            std::optional<std::vector<Entry>> owners;  // the entries of the marker, read when a name matches
            for (const auto& dep : pending->missingDependencies) {
                if (addedNames.count(fileNameKey(dep)) == 0)
                    continue;
                // A marker of an entry awaits the files of its own folder: the same file name in another folder
                // (the file of a copy of the model) does not release it, or the entry would be retried and
                // find the output of its twin complete without its own file. A marker of the layout before the
                // build key is the marker of every entry of the content, and releases by file name: a false match
                // only costs a retry
                if (!name->legacy()) {
                    if (!owners)
                        owners = entriesOfMarker(db, pendingPath, *name);
                    if (!anyOwnerAwaits(*owners, dep, addedPaths))
                        continue;
                }
                LOGD << "Releasing pending build " << io::toUtf8(pendingPath.filename()) << ": " << dep
                     << " may have been added";
                releasePendingMarker(pendingPath);
                break;
            }
        }
    } catch (const std::exception& ex) {
        LOGW << "Cannot release the pending builds that await added files: " << ex.what();
    }
}

std::vector<PendingBuildInfo> getPendingBuildInfo(Database* db) {
    std::vector<PendingBuildInfo> result;

    for (const auto& pendingPath : collectPendingFiles(db->buildDirectory())) {
        const auto name = parsePendingMarkerName(io::toUtf8(pendingPath.filename()));
        if (!name)
            continue;
        const PendingMarker pending = parsePendingFile(pendingPath);

        const std::vector<Entry> entries = entriesOfMarker(db, pendingPath, *name);
        if (entries.empty())
            continue;  // entry no longer indexed; garbage collected by cleanupBuild()

        PendingBuildInfo info;
        info.hash = entries.front().hash;
        info.key = name->key;
        info.path = entries.front().path;
        info.missingDependencies = pending.missingDependencies;
        info.lastAttempt = pending.lastAttempt;
        info.retryAfter = pendingRetryAfter(pending.lastAttempt);
        info.awaitingFiles = pending.awaitsFiles();

        result.push_back(std::move(info));
    }

    return result;
}

}  // namespace ddb
