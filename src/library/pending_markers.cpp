/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Pending markers: "<hash>.pending" records a build deferred until its missing
// dependencies are uploaded. Writing (atomically), parsing and listing markers,
// and retrying the deferred builds.

#include "build.h"

#include <cctype>
#include <cerrno>
#include <fstream>
#include <iterator>
#include <optional>

#ifdef _WIN32
#include <windows.h>
#endif

#include "build_internal.h"
#include "build_layout.h"
#include "cancellation.h"
#include "dbops.h"
#include "exceptions.h"
#include "mio.h"

namespace ddb {

namespace {

// Writes content into a sibling "<marker>.tmp-<rand16>" (see pendingMarkerTempInfix()) and
// renames it onto pendingPath, so a reader sees the old marker or the new one, never a part.
// Throws FSException; the sibling is removed and the previous marker kept on any failure
// (especially ENOSPC: a full disk must never remove the last pending marker).
void writeMarkerAtomically(const fs::path& pendingPath, const std::string& content) {
    const fs::path temporary =
        io::concat(pendingPath, PendingMarkerTempTag + utils::generateRandomString(16));
    try {
        std::ofstream out;
        errno = 0;
        out.open(temporary, std::ios::binary | std::ios::trunc);
        const int openError = errno;
        if (!out.is_open())
            throw FSException("Cannot open pending marker " + io::toUtf8(temporary),
                              std::error_code(openError, std::generic_category()));
        errno = 0;
        out << content;
        out.flush();
        const int writeError = errno;
        if (!out)
            throw FSException("Cannot write pending marker " + io::toUtf8(temporary),
                              std::error_code(writeError, std::generic_category()));
        errno = 0;
        out.close();
        const int closeError = errno;
        if (out.fail())
            throw FSException("Cannot close pending marker " + io::toUtf8(temporary),
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
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

}  // namespace

// Write the complete marker into a sibling file before replacing the old one.
// Failure (especially ENOSPC) must never remove the last pending marker.
bool replacePendingMarker(const fs::path& pendingPath, const std::string& dependencyLines) {
    try {
        writeMarkerAtomically(pendingPath, std::to_string(utils::currentUnixTimestamp()) + "\n" + dependencyLines);
        return true;
    } catch (const std::exception& ex) {
        LOGW << "Cannot refresh pending marker '" << io::toUtf8(pendingPath)
             << "'; original marker retained: " << ex.what();
        return false;
    }
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
        const time_t currentTime = utils::currentUnixTimestamp();
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
// relative to the source file's folder. They are informational only: a retry never matches
// them against the index.

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

// Retries every deferred build whose retry is due (pendingRetryAfter(), build_internal.h; force
// retries them all). A retry is a full build attempt: the dependency
// names stored in ".pending" are for display only and are never matched against
// the index (they used to be, which left models in subfolders pending forever).
// Each builder validates its dependencies on disk first; one still missing
// refreshes ".pending" (BuildDepMissingException, logged at debug). Classified
// primary transient failures keep an existing marker and refresh its timestamp;
// permanent failures consume it. Build-lock contention leaves it untouched.
// Failures remain logged at warning level for the next bounded coordinator/sweep.
void buildPending(Database* db, const std::string& outputPath, bool force) {
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

        const time_t now = utils::currentUnixTimestamp();
        const time_t retryAfter = pendingRetryAfter(pending.lastAttempt);
        if (!force && now < retryAfter) {
            LOGD << "Skipping build attempt for hash " << hash << " (last attempt "
                 << now - pending.lastAttempt << " seconds ago, retry after " << retryAfter << ")";
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
            // A canceled call stops before the next build (DQ-15)
            cancel::throwIfCanceled();
            try {
                LOGD << "Retrying pending build for " << e.path;
                buildInternal(db, e, outPath, force);
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

// Same marker set as buildPending(): top-level "<hash>.pending" files only.
bool isBuildPending(Database* db) {
    return !collectPendingFiles(db->buildDirectory()).empty();
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

        result.push_back(std::move(info));
    }

    return result;
}

}  // namespace ddb
