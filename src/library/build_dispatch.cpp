/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Build dispatch: one build attempt of an entry (lock, scratch folder, the
// build of its EntryHandler, publish) and the buildAll/build entry points.

#include "build.h"
#include "randomutil.h"

#include <cstdlib>
#include <fstream>
#include <optional>

#include "build_internal.h"
#include "build_layout.h"
#include "buildlock.h"
#include "cancellation.h"
#include "dbops.h"
#include "deppath.h"
#include "entry_handler.h"
#include "exceptions.h"
#include "index_sql.h"
#include "mio.h"
#include "process_config.h"
#include "raster_cache.h"

namespace ddb {

// Free disk space required on the build scratch filesystem before a build is started: the
// free-disk floor of the process configuration (DDBConfigure "minFreeDiskMb", else
// DDB_MIN_FREE_DISK_MB, else 512 MiB); 0 turns the check off.
void ensureSufficientDiskSpace(const fs::path& dir) {
    const uint64_t minMb = ProcessConfig::get().minFreeDiskMb();
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
            std::to_string(minMb) + " MB required on " + io::toUtf8(dir) +
            ". Free up space or lower minFreeDiskMb (DDBConfigure, DDB_MIN_FREE_DISK_MB).",
            RetryableBuildReason::LowDiskSpace);
    }
}

namespace {

// One build attempt of an entry under the lock of its output: the pending marker as the attempt
// found it, the scratch folder, and what to do with both when the attempt ends. run() is the
// attempt; fail() is called from a catch block with whatever it threw.
class BuildAttempt {
  public:
    BuildAttempt(Database* db, const Entry& target, const EntryHandler& handler, const fs::path& baseOutputPath,
                 const std::string& outputFolder, bool force, const BuildOptions& options)
        : db_(db), target_(target), handler_(handler), baseOutputPath_(baseOutputPath),
          outputFolder_(outputFolder), force_(force), options_(options),
          // Marker snapshot is read after exclusive primary ownership. Mutation waits
          // until serialized Model recovery has finished, so a busy tiles holder leaves
          // the original attempt timestamp untouched.
          pendFile_(pendingMarkerPath(baseOutputPath)),
          tempFolder_(outputFolder + TempFolderInfix + randomString(16)) {}

    void run() {
        try {
            wasPending_ = fs::is_regular_file(pendFile_);
            if (wasPending_) {
                std::ifstream pending(pendFile_, std::ios::binary);
                if (!pending.is_open())
                    throw FSException("Cannot read pending marker " + io::toUtf8(pendFile_));
                std::string timestamp;
                std::getline(pending, timestamp);
                pendingDependencyLines_.assign(std::istreambuf_iterator<char>(pending), {});
            }

            // An interrupted publish (Model: 3D Tiles swap) is repaired under the
            // primary lock, before the completeness check.
            handler_.recoverOutput(baseOutputPath_);

            // Recheck primary artifacts after serialized recovery. Success consumes
            // pending work even when the output was already complete before this attempt.
            if (handler_.isComplete(baseOutputPath_, false) && !force_) {
                LOGD << "Build output already exists after acquiring lock, skipping: " << outputFolder_;
                // A process that died after the publish, or a removal that failed then, left
                // the superseded output behind: it is removed here, under the same lock
                removeSupersededOutput();
                io::assureIsRemoved(pendFile_);
                return;
            }

            if (wasPending_)
                replacePendingMarker(pendFile_, pendingDependencyLines_);
            // Both preflight and scratch creation are inside the classified attempt.
            ensureSufficientDiskSpace(baseOutputPath_);
            io::assureFolderExists(io::fromUtf8(tempFolder_));

            const BuildContext context{db_,
                                       target_,
                                       db_->rootDirectory() / io::fromUtf8(target_.path),
                                       db_->rootDirectory(),
                                       baseOutputPath_,
                                       io::fromUtf8(outputFolder_),
                                       io::fromUtf8(tempFolder_),
                                       force_,
                                       options_};
            handler_.build(context);

            publish();
            // Only now, with the new output in place: a failed build keeps the old one
            removeSupersededOutput();

            io::assureIsRemoved(io::fromUtf8(tempFolder_));
            io::assureIsRemoved(pendFile_);
        } catch (const CanceledException&) {
            throw;
        } catch (...) {
            // A step that failed because the build was canceled (GDAL's "User terminated", a
            // killed tool) is the cancellation, never a permanent failure of the input
            cancel::throwIfCanceled();
            throw;
        }
    }

    // Decides what a failed attempt leaves behind and rethrows it (or its classified form): a
    // transient failure keeps the pending marker, a permanent one removes it. Only from a catch.
    [[noreturn]] void fail() {
        try {
            throw;
        } catch (const CanceledException&) {
            // Like a transient failure: a pending build stays pending
            retainPending();
            throw;
        } catch (const BuildDepMissingException& ex) {
            // Deferred success refreshes dependencies relative to the dataset root.
            // Atomic replacement also protects an existing marker if the disk is full.
            std::string dependencies;
            for (const auto& dep : ex.getMissingDependencies())
                dependencies += deppath::toDatasetRelative(target_.path, dep) + "\n";
            replacePendingMarker(pendFile_, dependencies);
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
            discardPending();
            throw;
        } catch (const fs::filesystem_error& ex) {
            if (ex.code() == std::errc::no_space_on_device) {
                retainPending();
                throw RetryableBuildException(ex.what(), RetryableBuildReason::NoSpaceOnDevice);
            }
            discardPending();
            throw AppException(ex.what());
        } catch (const AppException&) {
            discardPending();
            throw;
        } catch (...) {
            // Unknown third-party failures are permanent, never inferred from text.
            discardPending();
            throw AppException("Unknown build error failure for " + target_.path + " (" +
                               io::toUtf8(baseOutputPath_) + ")");
        }
    }

  private:
    // A handler that publishes its own outputs (Vector) skips the standard rename
    void publish() {
        if (!handler_.usesTempFolder())
            return;

        LOGD << "Build complete, moving temp folder to " << outputFolder_;
        if (fs::exists(io::fromUtf8(outputFolder_))) {
            // Rasters of the previous output kept open would block this on Windows
            releaseCachedFiles(io::fromUtf8(outputFolder_));
            io::assureIsRemoved(io::fromUtf8(outputFolder_));
        }

        io::assureFolderExists(io::fromUtf8(outputFolder_).parent_path());
        io::rename(io::fromUtf8(tempFolder_), io::fromUtf8(outputFolder_));
    }

    // The legacy output the published one replaces; best effort, it never fails the build
    void removeSupersededOutput() const {
        removeSupersededArtifacts(handler_, baseOutputPath_, filesystemBuildProbe());
    }

    void cleanupTemporary() {
        try {
            io::assureIsRemoved(io::fromUtf8(tempFolder_));
        } catch (const std::exception& cleanupError) {
            LOGW << "Cannot remove build scratch '" << tempFolder_ << "': " << cleanupError.what();
        }
    }

    void retainPending() {
        if (wasPending_)
            replacePendingMarker(pendFile_, pendingDependencyLines_);
        cleanupTemporary();
    }

    // The failure is permanent: the pending marker goes, with the scratch folder
    void discardPending() {
        io::assureIsRemoved(pendFile_);
        cleanupTemporary();
    }

    Database* db_;
    const Entry& target_;
    const EntryHandler& handler_;
    const fs::path baseOutputPath_;
    const std::string outputFolder_;
    const bool force_;
    const BuildOptions options_;
    const fs::path pendFile_;
    const std::string tempFolder_;
    bool wasPending_ = false;
    std::string pendingDependencyLines_;
};

}  // namespace

void buildInternal(Database* db, const Entry& e, const std::string& outputPath, bool force,
                   const BuildOptions& options) {
    std::string outPath = outputPath;
    if (outPath.empty())
        outPath = io::toUtf8(db->buildDirectory());

    LOGD << "Building entry " << e.path << " type " << entryTypeValue(e.type);
    cancel::throwIfCanceled();

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

    const fs::path baseOutputPath = io::fromUtf8(outPath) / target.hash;
    const std::string outputFolder = io::toUtf8(baseOutputPath / subfolder);

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

    BuildAttempt attempt(db, target, handler, baseOutputPath, outputFolder, force, options);
    try {
        attempt.run();
    } catch (...) {
        attempt.fail();
    }
}

void buildAll(Database* db, const std::string& outputPath, bool force, const BuildOptions& options) {
    std::string outPath = outputPath;
    if (outPath.empty())
        outPath = io::toUtf8(db->buildDirectory());

    LOGD << "In buildAll('" << outputPath << "')";

    // List all entries of the buildable types in DB
    const std::vector<EntryType> types = buildableEntryTypes();
    if (types.empty())
        return;
    auto q = db->query(selectEntries(EntryColumns::Properties) + " WHERE e.type IN " + inListSql(types.size()));
    bindEntryTypes(*q, 1, types);

    while (q->fetch()) {
        Entry e;
        readEntry(*q, EntryColumns::Properties, e);

        // Call build on each of them; a canceled call stops before the next one
        cancel::throwIfCanceled();
        try {
            buildInternal(db, e, outPath, force, options);
        } catch (const CanceledException&) {
            throw;
        } catch (const AppException& err) {
            LOGD << "Cannot build " << e.path << ": " << err.what();
        }
    }
}

void build(Database* db, const std::string& path, const std::string& outputPath, bool force,
           const BuildOptions& options) {
    LOGD << "In build('" << path << "','" << outputPath << "')";

    const auto e = getEntry(db, path);
    if (!e)
        throw InvalidArgsException(path + " is not a valid path in the database.");

    buildInternal(db, *e, outputPath, force, options);
}

}  // namespace ddb
