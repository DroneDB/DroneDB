/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// Adding to the index: plan (read-only), compute (hash and parse, no transaction), commit (short
// write transaction).
#include "dbops.h"
#include "dataset_walk.h"
#include "index_internal.h"

#include "build.h"
#include "build_internal.h"
#include "cad.h"
#include "entry_handler.h"
#include "entry_path.h"
#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "parallel.h"
#include "raster_cache.h"
#include "retrypolicy.h"
#include "status.h"
#include "transaction.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_map>

namespace ddb
{

    void to_json(json &j, const AddItemError &error)
    {
        j = json{{"path", error.path}, {"code", error.code}, {"message", error.message}};
    }

    void to_json(json &j, const AddResult &result)
    {
        j = json::object();
        j["entries"] = json::array();
        for (const auto &[entry, isUpdate] : result.entries) {
            json item = entry;
            item["status"] = isUpdate ? "updated" : "added";
            j["entries"].push_back(std::move(item));
        }

        j["unchanged"] = json::array();
        for (const auto &path : result.unchanged)
            j["unchanged"].push_back(json{{"path", path}});

        j["errors"] = result.errors;
    }

    const char *addErrorCode(const std::exception &ex) noexcept
    {
        if (dynamic_cast<const FSException *>(&ex)) return "FS";
        if (dynamic_cast<const GDALException *>(&ex)) return "GDAL";
        if (dynamic_cast<const PDALException *>(&ex)) return "PDAL";
        if (dynamic_cast<const JSONException *>(&ex)) return "JSON";
        if (dynamic_cast<const IndexException *>(&ex)) return "INDEX";
        return nullptr;
    }

    void doUpdate(Statement *updateQ, const Entry &e)
    {
        doUpdate(updateQ, e, static_cast<double>(e.mtime));
    }

    void doUpdate(Statement *updateQ, const Entry &e, double mtime)
    {
        // Fields (a whole-number mtime is stored as an integer, like before)
        updateQ->bind(1, e.hash);
        updateQ->bind(2, entryTypeValue(e.type));
        updateQ->bind(3, e.properties.dump());
        updateQ->bind(4, mtime);
        updateQ->bind(5, static_cast<long long>(e.size));
        updateQ->bind(6, e.depth);
        updateQ->bind(7, e.point_geom.toWkt());
        updateQ->bind(8, e.polygon_geom.toWkt());

        // Where
        updateQ->bind(9, e.path);

        updateQ->execute();
    }

    namespace {

    // Relative path -> the SHA-256 the caller already knows (and the size/mtime it saw, if any)
    using KnownHashes = std::unordered_map<std::string, KnownFile>;

    KnownHashes toKnownHashes(const std::vector<KnownFile> &files, const fs::path &directory) {
        KnownHashes known;
        for (const auto &f : files)
            if (!f.hash.empty())
                known[io::Path(io::fromUtf8(f.path)).relativeTo(directory).generic()] = f;
        return known;
    }

    std::vector<std::string> pathsOf(const std::vector<KnownFile> &files) {
        std::vector<std::string> paths;
        paths.reserve(files.size());
        for (const auto &f : files)
            paths.push_back(f.path);
        return paths;
    }

    // The known hash of `item`, or null when there is none or the file no longer has the
    // size/mtime the caller saw when it computed the hash (then the file is hashed as usual).
    // Sets `observedMtime` when it stats the file.
    const std::string *trustedHash(const KnownHashes *knownHashes, const std::string &relPath,
                                   const fs::path &absPath, std::optional<double> &observedMtime) {
        if (knownHashes == nullptr)
            return nullptr;
        const auto it = knownHashes->find(relPath);
        if (it == knownHashes->end())
            return nullptr;

        const KnownFile &known = it->second;
        if (known.size < 0 && known.mtime <= 0)
            return &known.hash;

        try {
            const auto state = io::Path(absPath).getState();
            if (state.directory ||
                (known.size >= 0 && static_cast<long long>(state.size) != known.size) ||
                (known.mtime > 0 && !io::sameModifiedTime(known.mtime, state.mtime))) {
                LOGD << "Hashing '" << relPath << "': it changed since its known hash was computed";
                return nullptr;
            }
            observedMtime = state.mtime;
        } catch (const FSException &) {
            return nullptr; // gone: the normal path reports it
        }
        return &known.hash;
    }

    // Result of the PLAN phase: what the index currently knows about a candidate path,
    // read without holding a write lock.
    struct PlannedAddItem {
        fs::path absPath;
        std::string relPath;
        bool existsInDb = false;
        double dbMtime = 0;
        std::string dbHash;
    };

    // Result of the COMPUTE phase: a fully parsed entry, ready to be written. Computed with
    // no transaction held so hashing/EXIF/GDAL/PDAL cost never blocks other writers.
    struct ComputedAddItem {
        Entry entry;
        bool isUpdate = false;
        double observedMtime = 0; // stored in the index (sub-second precision)
        long long observedSize = 0;
    };

    // PLAN: short, read-only pass over the index. No write lock is ever taken here.
    std::vector<PlannedAddItem> planAddCandidates(Database *db, const std::vector<fs::path> &pathList,
                                                  const fs::path &directory) {
        std::vector<PlannedAddItem> planned;
        planned.reserve(pathList.size());

        auto q = db->query("SELECT mtime,hash FROM entries WHERE path=?");

        for (auto &p : pathList) {
            io::Path relPath = io::Path(p).relativeTo(directory);

            if (!hasIndexableName(p)) {
                LOGD << "Skipping '" << p << "'";
                continue; // Skip file
            }

            PlannedAddItem item;
            item.absPath = p;
            item.relPath = relPath.generic();

            q->bind(1, item.relPath);
            if (q->fetch()) {
                item.existsInDb = true;
                item.dbMtime = q->getDouble(0);
                item.dbHash = q->getText(1);
            }
            q->reset();

            planned.push_back(std::move(item));
        }

        return planned;
    }

    // Threads wanted to hash and parse files while indexing (DDB_INDEX_THREADS: N, 0 = every
    // core, default min(8, cores); 1 = serial). Parsing is CPU work: the threads beyond the
    // calling one take CPU tokens (forEachIndex).
    size_t indexThreads() {
        return static_cast<size_t>(parallel::threadsOption("DDB_INDEX_THREADS", 8));
    }

    template <class T>
    void appendTo(std::vector<T> &target, std::vector<T> &&items) {
        target.insert(target.end(), std::make_move_iterator(items.begin()), std::make_move_iterator(items.end()));
    }

    // What COMPUTE made of a batch of planned items, each in input order: the entries to write,
    // the item-scoped failures (only when stopOnError is false) and the paths with nothing to write
    struct ComputedBatch {
        std::vector<ComputedAddItem> computed;
        std::vector<AddItemError> errors;
        std::vector<std::string> unchanged;
    };

    // What COMPUTE did with one planned item: an entry to write, no change, or an error
    struct ComputeOutcome {
        std::optional<ComputedAddItem> computed;
        std::optional<AddItemError> error;
        bool unchanged = false;
    };

    ComputeOutcome computeAddEntry(const PlannedAddItem &item, const fs::path &directory, bool stopOnError,
                                   const KnownHashes *knownHashes) {
        ComputeOutcome outcome;
        Entry e;
        bool add = false;
        std::optional<double> observedMtime;

        const std::string *knownHash = trustedHash(knownHashes, item.relPath, item.absPath, observedMtime);

        if (knownHash != nullptr) {
            e.hash = *knownHash;
            if (observedMtime)
                e.mtime = static_cast<time_t>(std::floor(*observedMtime));
            add = !item.existsInDb;
            if (!add && *knownHash == item.dbHash) {
                outcome.unchanged = true;
                return outcome;
            }
        } else if (item.existsInDb) {
            double fileMtime = 0;
            const auto status = checkUpdate(e, item.absPath, item.dbMtime, item.dbHash, &fileMtime);
            observedMtime = fileMtime;
            if (status == FileStatus::NotModified) {
                outcome.unchanged = true;
                return outcome;
            }
        } else {
            add = true;
        }

        const auto fail = [&](const char *code, const std::exception &ex) {
            if (stopOnError) throw;
            outcome.error = AddItemError{item.relPath, code, ex.what()};
            return outcome;
        };

        try {
            // Read before the content, so a write during the parse shows up at commit
            if (!observedMtime) {
                observedMtime = io::Path(item.absPath).getModifiedTimePrecise();
                e.mtime = static_cast<time_t>(std::floor(*observedMtime));
            }

            // parseEntry() skips re-hashing if checkUpdate() already set e.hash.
            parseEntry(item.absPath, directory, e, true);
        } catch (const AppException &ex) {
            // Only item-scoped failures become an item error; anything else aborts the batch.
            const char *code = addErrorCode(ex);
            if (code == nullptr) throw;
            return fail(code, ex);
        }

        ComputedAddItem ci;
        ci.isUpdate = !add;
        ci.observedMtime = *observedMtime;
        ci.observedSize = static_cast<long long>(e.size);
        ci.entry = std::move(e);
        outcome.computed = std::move(ci);
        return outcome;
    }

    // COMPUTE: hashing, fingerprinting, EXIF/GDAL/PDAL metadata extraction. No transaction is
    // held during this phase; this is the part that used to run for the whole duration of the
    // write lock. Items are independent, so they run on indexThreads() threads; the
    // results, errors and unchanged paths keep the input order.
    //
    // Item-scoped exceptions (FSException, GDALException, PDALException, JSONException,
    // IndexException) are captured into `errors` when `stopOnError` is false; with
    // `stopOnError` true they propagate and abort the whole call, matching addToIndex()'s
    // original all-or-nothing semantics (the exception of the first failed item in input
    // order, as a serial loop would). Database-scoped exceptions (DBException,
    // DBBusyException, bad_alloc, anything else) always propagate.
    //
    // `knownHashes` (relative path -> SHA-256) holds hashes the caller already trusts: those
    // files are never hashed, and an indexed one is updated when its hash differs.
    ComputedBatch computeAddEntries(const std::vector<PlannedAddItem> &planned,
                                    const fs::path &directory, bool stopOnError,
                                    const KnownHashes *knownHashes = nullptr) {
        std::vector<ComputeOutcome> outcomes(planned.size());
        parallel::forEachIndex(planned.size(), indexThreads(), [&](size_t i) {
            outcomes[i] = computeAddEntry(planned[i], directory, stopOnError, knownHashes);
        });

        ComputedBatch batch;
        batch.computed.reserve(planned.size());
        for (size_t i = 0; i < outcomes.size(); i++) {
            auto &outcome = outcomes[i];
            if (outcome.computed)
                batch.computed.push_back(std::move(*outcome.computed));
            else if (outcome.error)
                batch.errors.push_back(std::move(*outcome.error));
            else if (outcome.unchanged)
                batch.unchanged.push_back(planned[i].relPath);
        }

        return batch;
    }

    // What COMMIT did with the computed items: the entries written (Entry + isUpdate), the files
    // that changed under us (absolute paths, to re-plan) and the paths a concurrent writer had
    // already committed identically
    struct CommitOutcome {
        std::vector<std::pair<Entry, bool>> accepted;
        std::vector<fs::path> conflicts;
        std::vector<std::string> unchanged;
    };

    // Whether the file of a computed item changed (or is gone) since COMPUTE: re-validates its
    // (mtime, size) with one stat. Directories use logical size (0, or aggregate for nested
    // DroneDBs), so only the size of regular files is compared.
    bool changedSinceCompute(const fs::path &absPath, const ComputedAddItem &ci) {
        try {
            const auto state = io::Path(absPath).getState();
            const long long size = state.directory ? ci.observedSize : static_cast<long long>(state.size);
            return state.mtime != ci.observedMtime || size != ci.observedSize;
        } catch (const FSException &) {
            return true; // gone
        }
    }

    // INSERT of the entry (insertQ is the statement of commitAddEntries)
    void doInsert(Statement *insertQ, const Entry &e, double mtime) {
        insertQ->bind(1, e.path);
        insertQ->bind(2, e.hash);
        insertQ->bind(3, entryTypeValue(e.type));
        insertQ->bind(4, e.properties.dump());
        insertQ->bind(5, mtime);
        insertQ->bind(6, static_cast<long long>(e.size));
        insertQ->bind(7, e.depth);
        insertQ->bind(8, e.point_geom.toWkt());
        insertQ->bind(9, e.polygon_geom.toWkt());
        insertQ->execute();
    }

    // COMMIT: Transaction(Immediate); re-verify (mtime,size) to guard against a file that
    // changed between COMPUTE and COMMIT (TOCTOU), then
    // INSERT/UPDATE. Only row writes happen inside the lock — milliseconds, not seconds.
    // Items whose (mtime,size) changed since COMPUTE are reported via `conflicts` (absolute
    // paths) instead of being written with stale metadata; the caller decides whether to
    // re-plan them (addToIndexEx) or let them self-heal on the next add/rescan (addToIndex).
    CommitOutcome commitAddEntries(Database *db, const std::vector<ComputedAddItem> &computed,
                                   const fs::path &directory, const AddCallback &callback) {
        CommitOutcome outcome;
        if (computed.empty())
            return outcome;

        auto insertQ = db->query(
            "INSERT INTO entries (path, hash, type, properties, mtime, size, depth, "
            "point_geom, polygon_geom) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, GeomFromText(?, 4326), GeomFromText(?, "
            "4326))");
        const auto updateQ = db->query(kUpdateEntrySql);
        const auto recheckQ = db->query("SELECT mtime,hash FROM entries WHERE path=?");

        Transaction tx(db);

        // Buffered locally and only merged into the outcome after a successful commit, so a
        // canceled/rolled-back batch never reports rolled-back writes as committed.
        std::vector<std::pair<Entry, bool>> tentative;

        for (auto &ci : computed) {
            const fs::path absPath = directory / io::Path(io::fromUtf8(ci.entry.path)).get();

            // Re-validate filesystem state (mtime/size) between COMPUTE and COMMIT, with one
            // stat. Directories use logical size (0, or aggregate for nested DroneDBs), so
            // only the size of regular files is compared.
            if (changedSinceCompute(absPath, ci)) {
                LOGD << "Skipping '" << ci.entry.path
                    << "': changed between compute and commit (will be re-detected next add)";
                outcome.conflicts.push_back(absPath);
                continue;
            }

            // Re-query under the write transaction to handle the concurrent-add race.
            // The add/update decision was made in PLAN without a lock; another writer may
            // have committed a row with the same path before this transaction.
            recheckQ->bind(1, ci.entry.path);
            const bool stillAbsent = !recheckQ->fetch();
            const double dbMtime = stillAbsent ? 0 : recheckQ->getDouble(0);
            const std::string dbHash = stillAbsent ? "" : recheckQ->getText(1);
            recheckQ->reset();

            bool actuallyInsert = !ci.isUpdate;
            if (!stillAbsent && !ci.isUpdate) {
                // Planned as INSERT but row now exists (concurrent add) — convert to UPDATE
                if (io::sameModifiedTime(dbMtime, ci.observedMtime) && dbHash == ci.entry.hash) {
                    // Unchanged — a concurrent writer already committed the identical
                    // row. Report the path (null-guarded) so the completeness contract
                    // holds: every input lands in exactly one of entries/unchanged/errors.
                    outcome.unchanged.push_back(ci.entry.path);
                    continue;
                }
                actuallyInsert = false; // treat as UPDATE instead
            } else if (stillAbsent && ci.isUpdate) {
                // Planned as UPDATE but row was deleted — re-plan via conflict retry
                LOGD << "Skipping '" << ci.entry.path
                    << "': update plan stale, row no longer exists (will be re-planned)";
                outcome.conflicts.push_back(absPath);
                continue;
            }

            if (actuallyInsert) {
                doInsert(insertQ.get(), ci.entry, ci.observedMtime);
            } else {
                doUpdate(updateQ.get(), ci.entry, ci.observedMtime);
            }

            // Both report "updated", the opposite of "inserted"
            const bool updated = !actuallyInsert;
            tentative.push_back({ci.entry, updated});

            if (callback != nullptr)
                if (!callback(ci.entry, updated)) {
                    // tx's destructor rolls back; log it explicitly so a canceled batch
                    // is never a silent loss.
                    LOGD << "Add operation canceled by callback, rolling back transaction";
                    return outcome;
                }
        }

        tx.commit();

        outcome.accepted = std::move(tentative);
        return outcome;
    }

    } // namespace

    // A .prj gives its shapefile or CAD drawing a coordinate system.
    // Adding, changing or removing one changes the main file's footprint and
    // makes its built vector artifacts stale: re-parse the main entry and drop
    // those artifacts, so the next build (the .prj's own, or any later one)
    // redoes them. Runs after the add/remove transaction; best effort per file.
    void refreshPrjMains(Database *db, const fs::path &directory, const std::vector<std::string> &prjPaths,
                         bool removed) {
        if (prjPaths.empty())
            return;

        // A removed .prj is usually still on disk at this point (hosts delete the
        // file after the index entry): it must not give the drawing a CRS.
        std::vector<fs::path> ignored;
        if (removed)
            for (const auto &prj : prjPaths)
                ignored.push_back(directory / io::Path(io::fromUtf8(prj)).get());
        cad::IgnoredSidecarScope ignore(ignored);

        const auto updateQ = db->query(kUpdateEntrySql);
        for (const auto &prj : prjPaths) {
            for (const auto &mainPath : findSidecarMains(db, prj)) {
                const auto found = getEntry(db, mainPath);
                if (!found)
                    continue;
                const Entry &old = *found;

                try {
                    Entry fresh;
                    fresh.hash = old.hash; // content unchanged: skip re-hashing
                    parseEntry(directory / io::Path(io::fromUtf8(mainPath)).get(), directory, fresh, true);

                    {
                        Transaction tx(db);
                        doUpdate(updateQ.get(), fresh);
                        tx.commit();
                    }

                    // A GeoPackage kept open for WFS/OGC queries would block this on Windows
                    const fs::path out = db->buildDirectory() / old.hash;
                    releaseCachedFiles(out);
                    for (const auto &folder : entryHandler(old.type).artifactFolders(true))
                        io::assureIsRemoved(out / folder);
                    LOGD << "Refreshed " << mainPath << " after a change to " << prj;
                } catch (const std::exception &e) {
                    // The add/remove is already committed: never report it as failed
                    LOGW << "Cannot refresh " << mainPath << " after a change to " << prj << ": " << e.what();
                }
            }
        }
    }

    bool isPrjPath(const std::string &path) {
        return io::Path(io::fromUtf8(path)).checkExtension({"prj"});
    }

    namespace {

    // What follows a committed add of entries (added or updated): the main files of an added .prj
    // are refreshed, and the builds that await one of the added files are released
    void afterEntriesIndexed(Database *db, const fs::path &directory,
                             const std::vector<std::pair<Entry, bool>> &entries, size_t first = 0) {
        std::vector<std::string> prjPaths;
        std::vector<std::string> filePaths;
        for (size_t i = first; i < entries.size(); i++) {
            const Entry &e = entries[i].first;
            if (isPrjPath(e.path)) prjPaths.push_back(e.path);
            if (e.type != EntryType::Directory) filePaths.push_back(e.path);
        }
        refreshPrjMains(db, directory, prjPaths, false);
        releasePendingBuildsAwaiting(db, filePaths);
    }

    // addToIndex() with optional trusted hashes (see computeAddEntries)
    void addPathsToIndex(Database *db, const std::vector<std::string> &paths, const KnownHashes *knownHashes,
                         AddCallback callback)
    {
        if (paths.empty())
            return; // Nothing to do
        const fs::path directory = db->rootDirectory();
        // Index exactly what was asked when the caller knows the hashes (applyDelta lists every
        // path it adds): walking a folder could pick up files it did not add.
        auto pathList = getIndexPathList(directory, paths, true, /*recursive=*/knownHashes == nullptr);

        // stopOnError: nothing lands in errors; unchanged and conflicting paths are not reported
        const auto planned = planAddCandidates(db, pathList, directory);
        const auto batch = computeAddEntries(planned, directory, /*stopOnError=*/true, knownHashes);
        const auto committed = commitAddEntries(db, batch.computed, directory, callback);
        afterEntriesIndexed(db, directory, committed.accepted);
    }

    } // namespace

    void addToIndex(Database *db, const std::vector<std::string> &paths,
                    AddCallback callback)
    {
        addPathsToIndex(db, paths, nullptr, callback);
    }

    void addToIndexWithHashes(Database *db, const std::vector<KnownFile> &files, AddCallback callback)
    {
        const auto knownHashes = toKnownHashes(files, db->rootDirectory());
        addPathsToIndex(db, pathsOf(files), &knownHashes, callback);
    }

    // addToIndexEx() with optional known hashes (see computeAddEntries)
    static void addPathsToIndexEx(Database *db, const std::vector<std::string> &paths, const AddOptions &options,
                                  AddResult &result, AddCallback callback, const KnownHashes *knownHashes)
    {
        if (paths.empty())
            return; // Nothing to do
        const fs::path directory = db->rootDirectory();
        // Files with known hashes are indexed exactly as listed (see addPathsToIndex)
        auto pathList = getIndexPathList(directory, paths, true, /*recursive=*/knownHashes == nullptr);

        // Clamp negative retries: a negative value would skip the loop entirely
        // and report every item as a CONFLICT without attempting indexing.
        const int maxRetries = options.maxConflictRetries < 0 ? AddOptions{}.maxConflictRetries : options.maxConflictRetries;
        const size_t firstNewEntry = result.entries.size();

        for (int pass = 0; pass <= maxRetries && !pathList.empty(); ++pass) {
            const auto planned = planAddCandidates(db, pathList, directory);
            auto batch = computeAddEntries(planned, directory, options.stopOnError, knownHashes);
            // Reported even when the commit throws: they are final
            appendTo(result.errors, std::move(batch.errors));
            appendTo(result.unchanged, std::move(batch.unchanged));

            auto committed = commitAddEntries(db, batch.computed, directory, callback);
            appendTo(result.entries, std::move(committed.accepted));
            appendTo(result.unchanged, std::move(committed.unchanged));

            pathList = std::move(committed.conflicts); // bounded re-plan: only what actually changed under us
        }

        for (auto &p : pathList) {
            io::Path relPath = io::Path(p).relativeTo(directory);
            result.errors.push_back({relPath.generic(), "CONFLICT", "File kept changing during indexing"});
        }

        afterEntriesIndexed(db, directory, result.entries, firstNewEntry);
    }

    void addToIndexEx(Database *db, const std::vector<std::string> &paths,
                      const AddOptions &options, AddResult &result, AddCallback callback)
    {
        addPathsToIndexEx(db, paths, options, result, callback, nullptr);
    }

    void addToIndexEx(Database *db, const std::vector<KnownFile> &files,
                      const AddOptions &options, AddResult &result, AddCallback callback)
    {
        const auto knownHashes = toKnownHashes(files, db->rootDirectory());
        addPathsToIndexEx(db, pathsOf(files), options, result, callback, &knownHashes);
    }

} // namespace ddb
