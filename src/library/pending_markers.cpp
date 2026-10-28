/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Pending markers: "<hash>.pending" records a build deferred until its missing
// dependencies are uploaded. Writing (atomically), parsing and listing markers,
// releasing those that await an added file, and retrying the deferred builds.

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
    const size_t infixAt = 64;
    if (name.size() <= infixAt + infix.size())
        return false;
    if (name.compare(infixAt, infix.size(), infix) != 0)
        return false;
    return isHashLike(name.substr(0, infixAt));
}

namespace {

// Format of a ".pending" marker file, created by buildInternal() when a build is deferred due
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
        if (!isHashLike(io::toUtf8(p.stem())))
            continue;
        pendingFiles.push_back(p);
    }
    return pendingFiles;
}

}  // namespace

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
    if (!fs::exists(buildDir, ec) || ec)
        return;

    const std::string outPath = io::toUtf8(buildDir);

    // Collect first: building creates and removes files in this directory, which
    // must not happen under a live directory iterator.
    const std::vector<fs::path> pendingFiles = collectPendingFiles(buildDir);

    for (const auto& pendingPath : pendingFiles) {
        const auto hash = io::toUtf8(pendingPath.stem());
        const PendingMarker pending = parsePendingFile(pendingPath);

        if (!force && pending.awaitsFiles()) {
            LOGD << "Skipping build attempt for hash " << hash << " (awaits "
                 << pending.missingDependencies.size() << " missing file(s))";
            continue;
        }

        const time_t now = currentUnixTimestamp();
        const time_t retryAfter = pendingRetryAfter(pending.lastAttempt);
        if (!force && now < retryAfter) {
            LOGD << "Skipping build attempt for hash " << hash << " (last attempt "
                 << now - pending.lastAttempt << " seconds ago, retry after " << retryAfter << ")";
            continue;
        }

        std::vector<Entry> entries;
        {
            auto q = db->query(selectEntries(EntryColumns::Properties) + " WHERE e.hash = ?");
            q->bind(1, hash);
            while (q->fetch()) {
                Entry e;
                readEntry(*q, EntryColumns::Properties, e);
                entries.push_back(std::move(e));
            }
        }

        if (entries.empty()) {
            // Entry no longer indexed: nothing to retry (cleanupBuild garbage collects it too)
            LOGD << "Removing pending file of unindexed hash " << hash;
            io::assureIsRemoved(pendingPath);
            continue;
        }

        for (const auto& e : entries) {
            // A canceled call stops before the next build
            cancel::throwIfCanceled();
            try {
                LOGD << "Retrying pending build for " << e.path;
                buildInternal(db, e, outPath, force, options);
            } catch (const CanceledException&) {
                throw;
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

// Same marker set as buildPending(): top-level "<hash>.pending" files only, and only those a
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

        std::unordered_set<std::string> added;
        for (const auto& p : filePaths)
            added.insert(fileNameKey(p));

        for (const auto& pendingPath : pendingFiles) {
            const auto pending = readPendingMarker(pendingPath);
            if (!pending || !pending->awaitsFiles())
                continue;
            for (const auto& dep : pending->missingDependencies) {
                if (added.count(fileNameKey(dep)) == 0)
                    continue;
                LOGD << "Releasing pending build " << io::toUtf8(pendingPath.stem()) << ": " << dep
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
        const auto hash = io::toUtf8(pendingPath.stem());
        const PendingMarker pending = parsePendingFile(pendingPath);

        auto q = db->query("SELECT path FROM entries WHERE hash = ?");
        q->bind(1, hash);
        if (!q->fetch())
            continue;  // entry no longer indexed; garbage collected by cleanupBuild()

        PendingBuildInfo info;
        info.hash = hash;
        info.path = q->getText(0);
        info.missingDependencies = pending.missingDependencies;
        info.lastAttempt = pending.lastAttempt;
        info.retryAfter = pendingRetryAfter(pending.lastAttempt);
        info.awaitingFiles = pending.awaitsFiles();

        result.push_back(std::move(info));
    }

    return result;
}

}  // namespace ddb
