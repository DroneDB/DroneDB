/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// Changing the index: removal, sync with the filesystem, rescan, and moving entries.
#include "dbops.h"
#include "index_internal.h"

#include "build.h"
#include "build_internal.h"
#include "build_layout.h"
#include "entry_filters.h"
#include "entry_path.h"
#include "exceptions.h"
#include "index_sql.h"
#include "logger.h"
#include "mio.h"
#include "parallel.h"
#include "raster_cache.h"
#include "retrypolicy.h"
#include "sog_layout.h"
#include "status.h"
#include "transaction.h"

#include <ctime>
#include <map>
#include <memory>
#include <optional>
#include <set>

namespace ddb
{

    namespace {

    void removeBuildFolder(const fs::path &buildFolder)
    {
        if (fs::exists(buildFolder))
        {
            LOGD << "Removing " << io::toUtf8(buildFolder);
            releaseCachedFiles(buildFolder);
            io::assureIsRemoved(buildFolder);
        }
    }

    // The key of the build an entry has before a rescan (D-020, see rescanIndex): the key its index row names
    // by the rules of the build key. The folders written before the build key are named by the hash alone:
    // a row whose key is another name, with no folder of that key but a folder of its hash, still has its
    // build in the folder of its hash, and the rescan reports the move. With neither folder (a build that is
    // deferred or keeps failing) there is nothing to move: the key of the row is the key before, so the entry
    // is not reported again at every rescan.
    std::string buildKeyBeforeRescan(const fs::path &buildDirectory, const std::string &hash, EntryType type,
                                     const json &properties)
    {
        const std::string key = buildKeyOfRow(hash, type, properties);
        if (key == hash)
            return key;
        const auto isFolder = [&](const std::string &name)
        {
            std::error_code ec;
            return fs::is_directory(buildDirectory / io::fromUtf8(name), ec) && !ec;
        };
        return !isFolder(key) && isFolder(hash) ? hash : key;
    }

    // Deletes the entries matching a predicate, and their metadata, inside the caller's
    // transaction. The statements are prepared once for one predicate sql and bound again
    // for each run, so removing many paths does not prepare three statements per path.
    class RowDeleter
    {
        BuildPropertyColumns properties_;
        std::unique_ptr<Statement> metaDelete_, select_, delete_;

    public:
        RowDeleter(Database *db, const std::string &predicateSql)
            : metaDelete_(db->query("DELETE FROM entries_meta WHERE " + predicateSql)),
              select_(db->query("SELECT path, hash, type, " + properties_.sql() + " FROM entries WHERE " +
                                predicateSql)),
              delete_(db->query("DELETE FROM entries WHERE " + predicateSql))
        {
        }

        // `predicate` has the sql given to the constructor. Appends the deleted paths and
        // the build keys of those with a hash (the names of their build folders); returns the count.
        int run(const PathPredicate &predicate, std::vector<std::string> &deletedPaths,
                std::vector<std::string> &deletedHashes)
        {
            bindPathPredicate(*metaDelete_, predicate);
            metaDelete_->execute();

            bindPathPredicate(*select_, predicate);
            int count = 0;
            while (select_->fetch())
            {
                deletedPaths.push_back(select_->getText(0));
                const auto hash = select_->getText(1);
                if (!hash.empty())
                {
                    const auto type = static_cast<EntryType>(select_->getInt(2));
                    deletedHashes.push_back(buildKeyOfRow(hash, type, properties_.read(*select_, 3, type)));
                }
                count++;
            }
            select_->reset();

            if (count > 0)
            {
                bindPathPredicate(*delete_, predicate);
                delete_->execute();
            }

            return count;
        }
    };

    // Path and type of the entries `query` selects (see buildPathPredicate), without the
    // metadata and geometries getMatchingEntries() reads
    std::vector<std::pair<std::string, EntryType>> matchingPathsAndTypes(Database *db, const std::string &query)
    {
        const auto predicate = buildPathPredicate(query, false);
        auto q = db->query("SELECT path, type FROM entries" +
                           (predicate.sql.empty() ? std::string() : " WHERE " + predicate.sql));
        bindPathPredicate(*q, predicate);

        std::vector<std::pair<std::string, EntryType>> matches;
        while (q->fetch())
            matches.emplace_back(q->getText(0), static_cast<EntryType>(q->getInt(1)));
        q->reset();
        return matches;
    }

    // Removes the build folders (named by build key) of deleted entries, on the I/O threads (ioThreads),
    // outside any transaction.
    void deleteBuildFolders(Database *db, const std::vector<std::string> &keys)
    {
        if (keys.empty())
            return;

        const auto buildDir = db->buildDirectory();
        parallel::forEachIndexIo(keys.size(), [&](size_t i)
                                 {
            try
            {
                removeBuildFolder(buildDir / io::fromUtf8(keys[i]));
            }
            catch (const std::exception &e)
            {
                LOGD << "Warning: Failed to delete build folder: " << e.what();
            } });
    }

    } // namespace

    void checkDeleteBuild(Database *db, const std::string &key)
    {
        if (!key.empty())
            removeBuildFolder(db->buildDirectory() / io::fromUtf8(key));
    }

    void checkDeleteMeta(Database *db, const std::string &path)
    {
        if (!path.empty())
        {
            auto q = db->query("DELETE FROM entries_meta WHERE path = ?");
            q->bind(1, path);
            executeWithRetry([&q] { q->execute(); }, "checkDeleteMeta");
        }
    }

    void removeFromIndex(Database *db, const std::vector<std::string> &paths, RemoveCallback callback)
    {
        if (paths.empty())
        {
            // Nothing to do
            LOGD << "No paths provided";
            return;
        }

        const fs::path directory = db->rootDirectory();

        std::vector<std::string> deletedPaths, deletedHashes, removedPrj;

        {
            // One transaction for the whole list: a path without entries removes nothing.
            Transaction tx(db);

            // Every path must match before anything is removed: a path listed twice, or
            // inside a folder also listed, is then removed once instead of failing the
            // second time. Sorted, so a folder comes before its content.
            std::map<std::string, EntryType> targets;
            for (const auto &p : paths)
            {
                LOGD << "Deleting path: " << p;

                auto relPath = io::Path(io::fromUtf8(p)).relativeTo(directory);

                LOGD << "Rel path: " << relPath.generic();

                const auto matches = matchingPathsAndTypes(db, relPath.generic());
                if (matches.empty())
                    throw FSException("No matching entries");
                targets.insert(matches.begin(), matches.end());
            }

            RowDeleter exact(db, "path = ?");
            RowDeleter descendants(db, descendantsPredicate("", "path").sql);

            for (const auto &[path, type] : targets)
            {
                const auto cnt = exact.run({"path = ?", {path}}, deletedPaths, deletedHashes);
                if (cnt > 0 && type != EntryType::Directory && isPrjPath(path))
                    removedPrj.push_back(path);

                if (type == EntryType::Directory && !path.empty())
                    descendants.run(descendantsPredicate(path, "path"), deletedPaths, deletedHashes);
            }

            tx.commit();
        }

        // Reported once committed, so a rolled-back removal is never announced
        if (callback != nullptr)
            for (const auto &path : deletedPaths)
                callback(path);

        deleteBuildFolders(db, deletedHashes);
        refreshPrjMains(db, directory, removedPrj, true);
    }

    void syncIndex(Database *db, SyncCallback callback)
    {
        const fs::path directory = db->rootDirectory();

        const BuildPropertyColumns properties;
        auto q = db->query("SELECT path, mtime, hash, type, " + properties.sql() + " FROM entries");
        auto deleteQ = db->query("DELETE FROM entries WHERE path = ?");
        const auto updateQ = db->query(updateEntrySql());

        // Deliberately one IMMEDIATE transaction for the whole re-index (design choice, kept):
        // the entries table is always left in a consistent, fully re-parsed state, even for
        // consumers reading between statements. The cost is that re-index holds the sole SQLite
        // writer lock for its entire duration; concurrent addToIndexEx callers see busy and
        // retry/jitter per RetryPolicy.
        Transaction tx(db);

        while (q->fetch())
        {
            io::Path relPath = io::fromUtf8(q->getText(0));
            fs::path p = directory / relPath.get();
            Entry e;
            const auto mtime = q->getDouble(1);
            const auto hash = q->getText(2);
            const auto type = static_cast<EntryType>(q->getInt(3));
            const json buildProperties = properties.read(*q, 4, type);
            double fileMtime = 0;
            const auto status = checkUpdate(e, p, mtime, hash, &fileMtime);

            switch (status)
            {

            case Deleted:
                // Removed
                deleteQ->bind(1, relPath.generic());
                deleteQ->execute();
                if (!hash.empty())
                    checkDeleteBuild(db, buildKeyOfRow(hash, type, buildProperties));
                checkDeleteMeta(db, relPath.generic());
                if (callback)
                    callback(true, relPath.generic());
                break;

            case Modified:

                parseEntry(p, directory, e, true);
                doUpdate(updateQ.get(), e, fileMtime);
                if (callback)
                    callback(false, e.path);
                break;

            default:; // Do nothing
            }
        }

        tx.commit();
    }

    namespace {

    // The paths a rescan re-reads (sorted, once each) and the paths of the scope that are not in the index
    struct RescanSelection
    {
        std::vector<std::string> paths;
        std::vector<std::string> unknown;
    };

    // The rows of the scope's types (never a directory: it has no file metadata to re-read), and of
    // `predicate` when it is not empty
    std::vector<std::string> selectRescanRows(Database *db, const std::vector<EntryType> &types,
                                              const PathPredicate &predicate)
    {
        SqlConditions conditions;
        conditions.add(typeCondition("type", "!=", EntryType::Directory));
        if (!types.empty())
            conditions.addEntryTypes("type", types);
        conditions.add(predicate);

        auto q = db->query("SELECT path FROM entries" + conditions.whereClause() + " ORDER BY path");
        conditions.bind(*q);
        std::vector<std::string> paths;
        while (q->fetch())
            paths.push_back(q->getText(0));
        q->reset();
        return paths;
    }

    RescanSelection selectRescanPaths(Database *db, const RescanScope &scope)
    {
        RescanSelection selection;
        if (scope.paths.empty())
            selection.paths = selectRescanRows(db, scope.types, {});
        else
        {
            const auto typeQ = db->query("SELECT type FROM entries WHERE path = ?");
            std::set<std::string> paths;
            for (const auto &path : scope.paths)
            {
                typeQ->bind(1, path);
                const bool indexed = typeQ->fetch();
                const bool folder = indexed && static_cast<EntryType>(typeQ->getInt(0)) == EntryType::Directory;
                typeQ->reset();
                if (!indexed)
                {
                    selection.unknown.push_back(path);
                    continue;
                }
                const PathPredicate predicate =
                    folder ? descendantsPredicate(path, "path") : PathPredicate{"path = ?", {path}};
                for (auto &row : selectRescanRows(db, scope.types, predicate))
                    paths.insert(std::move(row));
            }
            selection.paths.assign(paths.begin(), paths.end());
        }

        if (!scope.extensions.empty())
            std::erase_if(selection.paths, [&](const std::string &path)
                          { return !filters::hasAnyExtension(path, scope.extensions); });
        return selection;
    }

    // Re-reads entries one at a time and tells the callbacks. Whether a row is written in the caller's
    // transaction or in one of its own is the caller's (see rescanIndex).
    class EntryRescanner
    {
        Database *db_;
        fs::path directory_;
        fs::path buildDirectory_;
        bool stopOnError_;
        RescanCallback callback_;
        RescanKeyCallback keyCallback_;
        std::unique_ptr<Statement> updateQ_;
        // What the index holds of an entry before it is re-read: the build key reads it
        BuildPropertyColumns buildProperties_;
        std::unique_ptr<Statement> beforeQ_;
        // The folders of an unbundled SOG splat are looked at once for the whole rescan
        std::shared_ptr<SogFolderCache> sogFolders_ = std::make_shared<SogFolderCache>();

        // The key of the build the row of `path` names; nothing when the row is gone
        std::optional<std::string> keyBefore(const std::string &path)
        {
            beforeQ_->bind(1, path);
            std::optional<std::string> key;
            if (beforeQ_->fetch())
            {
                const auto type = static_cast<EntryType>(beforeQ_->getInt(1));
                key = buildKeyBeforeRescan(buildDirectory_, beforeQ_->getText(0), type,
                                           buildProperties_.read(*beforeQ_, 2, type));
            }
            beforeQ_->reset();
            return key;
        }

        // The entry of the file, parsed again from scratch (its hash too)
        Entry parsed(const fs::path &fullPath)
        {
            Entry e;
            FingerprintContext ctx;
            ctx.sogFolders = sogFolders_;
            parseEntry(fullPath, directory_, e, true, ctx);
            return e;
        }

        // Tells the callbacks of the re-read entry; false when the callback cancels
        bool written(const Entry &e, const std::string &keyBefore)
        {
            if (keyCallback_ != nullptr)
            {
                const std::string keyAfter = entryHandler(e.type).buildKey(e);
                keyCallback_(e, keyAfter, keyAfter != keyBefore);
            }
            return callback_ == nullptr || callback_(e, true, "");
        }

        // Tells the callback that `path` failed; false when it cancels
        bool reported(const std::string &path, const std::string &error)
        {
            if (callback_ == nullptr)
                return true;
            Entry e;
            e.path = path;
            return callback_(e, false, error);
        }

        // Runs `reread` on the file of `path` and reports what fails: a missing file, or what `reread`
        // throws (with stopOnError that exception goes on, so a stop throws what failed). False when the
        // callback cancels
        template <typename Reread> bool rescan(const std::string &path, Reread reread)
        {
            const fs::path fullPath = directory_ / io::Path(io::fromUtf8(path)).get();
            if (!fs::exists(fullPath))
                return failed(path, FSException("File not found: " + io::toUtf8(fullPath)));

            try
            {
                return reread(fullPath);
            }
            catch (const std::exception &ex)
            {
                LOGD << "Error processing " << fullPath << ": " << ex.what();
                if (!reported(path, ex.what()))
                    return false;
                if (stopOnError_)
                    throw;
                return true;
            }
        }

    public:
        EntryRescanner(Database *db, bool stopOnError, RescanCallback callback, RescanKeyCallback keyCallback)
            : db_(db), directory_(db->rootDirectory()), buildDirectory_(db->buildDirectory()),
              stopOnError_(stopOnError), callback_(std::move(callback)), keyCallback_(std::move(keyCallback)),
              updateQ_(db->query(updateEntrySql())),
              beforeQ_(db->query("SELECT hash, type, " + buildProperties_.sql() + " FROM entries WHERE path = ?"))
        {
        }

        // Reports that `path` failed with `error`; false when the callback cancels. With stopOnError,
        // throws `error`
        template <typename Error> bool failed(const std::string &path, const Error &error)
        {
            LOGD << "Cannot rescan " << path << ": " << error.what();
            if (!reported(path, error.what()))
                return false;
            if (stopOnError_)
                throw error;
            return true;
        }

        // Re-reads `path` inside the caller's transaction; false when the callback cancels
        bool rescanInTransaction(const std::string &path)
        {
            return rescan(path, [&](const fs::path &fullPath)
            {
                const std::string before = keyCallback_ != nullptr ? keyBefore(path).value_or("") : "";
                Entry e = parsed(fullPath);
                doUpdate(updateQ_.get(), e);
                return written(e, before);
            });
        }

        // Re-reads `path` outside the writer lock and writes it in a transaction of its own, unless its
        // file or its row changed meanwhile; false when the callback cancels
        bool rescanOwnTransaction(const std::string &path)
        {
            return rescan(path, [&](const fs::path &fullPath)
            {
                const double observedMtime = io::Path(fullPath).getState().mtime;
                Entry e = parsed(fullPath);

                Transaction tx(db_);
                if (fileChangedSince(fullPath, observedMtime, static_cast<long long>(e.size)))
                    throw FSException("Changed while it was re-read: " + io::toUtf8(fullPath));
                const auto before = keyBefore(path);
                if (!before)
                    throw EntryNotFoundException("Removed from the index while it was re-read: " + path);
                // The time the file had when it was read, with its sub-second part, as an add writes it
                doUpdate(updateQ_.get(), e, observedMtime);
                tx.commit();
                return written(e, *before);
            });
        }
    };

    } // namespace

    void rescanIndex(Database *db, const RescanScope &scope, bool stopOnError, RescanCallback callback,
                     RescanKeyCallback keyCallback)
    {
        EntryRescanner rescanner(db, stopOnError, std::move(callback), std::move(keyCallback));

        if (!scope.paths.empty())
        {
            const auto selection = selectRescanPaths(db, scope);
            for (const auto &path : selection.unknown)
                if (!rescanner.failed(path, EntryNotFoundException("Not in index: " + path)))
                    return;
            for (const auto &path : selection.paths)
                if (!rescanner.rescanOwnTransaction(path))
                    return;
            return;
        }

        // Deliberately one IMMEDIATE transaction for the whole rescan (design choice, kept):
        // the entries table is always left in a consistent, fully re-parsed state, even for
        // consumers reading between statements. The cost is that re-scan holds the sole SQLite
        // writer lock for its entire duration; concurrent addToIndexEx callers see busy and
        // retry/jitter per RetryPolicy. A cancel or a stop returns early: tx's destructor rolls back.
        Transaction tx(db);
        for (const auto &path : selectRescanPaths(db, scope).paths)
            if (!rescanner.rescanInTransaction(path))
                return;
        tx.commit();

        // The markers of the layout before the build key become those of their entries, with the keys
        // the rescan gave them
        migrateLegacyPendingMarkers(db, db->buildDirectory());
    }

    // Sets the modified times of files in the filesystem
    // to match that of the database. Optionally,
    // if a whitelist of files is specified, limits the scope of the synchronization
    // to those files only. An empty set of files indicates to update all files.
    void syncLocalMTimes(Database *db, const std::vector<std::string> &files)
    {
        const fs::path directory = db->rootDirectory();
        SqlConditions conditions;
        conditions.add(typeCondition("type", "!=", EntryType::Directory));
        conditions.add(typeCondition("type", "!=", EntryType::DroneDB));
        if (!files.empty())
            conditions.addInStrings("path", files);

        auto q = db->query("SELECT path,mtime FROM entries" + conditions.whereClause());
        conditions.bind(*q);

        while (q->fetch())
        {
            io::Path fullPath = directory / io::fromUtf8(q->getText(0));
            if (fullPath.setModifiedTimePrecise(q->getDouble(1)))
            {
                LOGD << "Updated mtime for " << io::toUtf8(fullPath.get());
            }
        }
    }

    void deleteEntry(Database *db, const std::string &path)
    {

        auto f = db->query("DELETE FROM entries WHERE path = ?");
        f->bind(1, path);
        executeWithRetry([&f] { f->execute(); }, "deleteEntry");

        checkDeleteMeta(db, path);
    }

    void addFolder(Database *db, const std::string path, const time_t mtime)
    {
        const auto q = db->query(insertFolderSql());
        q->bind(1, path);
        q->bind(2, static_cast<long long>(mtime));
        q->bind(3, ddb::io::Path(io::fromUtf8(path)).depth());
        executeWithRetry([&q] { q->execute(); }, "addFolder");
    }

    void createMissingFolders(Database *db)
    {

        const auto q = db->query(missingFoldersSql());

        while (q->fetch())
        {

            const auto folder = q->getText(0);

            LOGD << "Creating missing folder '" << folder << "'";

            addFolder(db, folder, time(NULL));
        }
    }

    std::vector<std::string> listFolderPaths(Database *db, const std::string &path)
    {

        std::vector<std::string> res;

        if (path.empty())
            return res;

        const auto descendants = buildPathPredicate(path, true);
        auto q = db->query("SELECT path FROM entries WHERE path = ? OR (" + descendants.sql + ")");

        q->bind(1, path);
        bindPathPredicate(*q, descendants, 2);

        while (q->fetch())
        {

            const auto p = q->getText(0);

            res.push_back(p);
        }

        return res;
    }

    namespace
    {

        // The paths of the files below folder (folders excluded)
        std::vector<std::string> listFilePaths(Database *db, const std::string &folder)
        {
            const auto descendants = buildPathPredicate(folder, true);
            auto q = db->query("SELECT path FROM entries WHERE " + typeCondition("type", "!=", EntryType::Directory) +
                               " AND (" + descendants.sql + ")");
            bindPathPredicate(*q, descendants, 1);
            std::vector<std::string> res;
            while (q->fetch())
                res.push_back(q->getText(0));
            return res;
        }

        // The build key the index row at path names (build_layout.h), nothing for a folder or a path not indexed
        std::optional<std::string> buildKeyAt(Database *db, const std::string &path)
        {
            const BuildPropertyColumns properties;
            auto q = db->query("SELECT hash, type, " + properties.sql() + " FROM entries WHERE path = ?");
            q->bind(1, path);
            if (!q->fetch() || q->getText(0).empty())
                return std::nullopt;
            const auto type = static_cast<EntryType>(q->getInt(1));
            return buildKeyOfRow(q->getText(0), type, properties.read(*q, 2, type));
        }

        // A file moved from oldPath (whose build key was oldKey) to newPath: its pending marker is named by its
        // path (build_layout.h), so it follows the file, or the moved entry would lose it and a cleanup would
        // remove it as the marker of no entry. The names a marker that awaits files holds are those of the old
        // place: it is released, so the next retry looks for the files of the new place and names them. A
        // marker left at the new name by an entry that had that path (replaced by the move) is of no entry now.
        // Failures are logged: the index is committed already.
        void movePendingMarker(Database *db, const std::string &oldPath, const std::optional<std::string> &oldKey,
                               const std::string &newPath)
        {
            const auto newKey = buildKeyAt(db, newPath);
            if (!newKey)
                return;
            const fs::path buildDirectory = db->buildDirectory();
            const fs::path to = pendingMarkerPath(buildDirectory, *newKey, newPath);
            std::error_code ec;
            if (oldKey && *oldKey == *newKey)
            {
                const fs::path from = pendingMarkerPath(buildDirectory, *oldKey, oldPath);
                if (fs::is_regular_file(from, ec) && !ec)
                {
                    fs::rename(from, to, ec);
                    if (ec)
                    {
                        LOGW << "Cannot move the pending marker of " << oldPath << " to " << newPath << ": "
                             << ec.message();
                        return;
                    }
                    const auto marker = readPendingMarker(to);
                    if (marker && marker->awaitsFiles())
                        releasePendingMarker(to);
                    return;
                }
            }
            if (fs::is_regular_file(to, ec) && !ec)
            {
                LOGD << "Removing the pending marker " << io::toUtf8(to.filename()) << " of the entry " << newPath
                     << " replaced by a move";
                fs::remove(to, ec);
                if (ec)
                {
                    LOGW << "Cannot remove the pending marker '" << io::toUtf8(to) << "': " << ec.message();
                }
            }
        }

    } // namespace

    void replacePath(Database *db, const std::string &source, const std::string &dest)
    {

        LOGD << "Replacing '" << source << "' to '" << dest << "'";

        const auto depth = io::Path(io::fromUtf8(dest)).depth();

        auto update = db->query("UPDATE entries SET path = ?, depth = ? WHERE path = ?");
        update->bind(1, dest);
        update->bind(2, depth);
        update->bind(3, source);
        executeWithRetry([&update] { update->execute(); }, "replacePath");

        // Move meta
        auto mq = db->query("UPDATE entries_meta SET path = ? WHERE path = ?");
        mq->bind(1, dest);
        mq->bind(2, source);
        executeWithRetry([&mq] { mq->execute(); }, "replacePath meta");
    }

    void moveEntry(Database *db, const std::string &source, const std::string &dest)
    {

        if (source[source.length() - 1] == '/' || source[source.length() - 1] == '\\')
            throw InvalidArgsException("source cannot end with path separator");

        if (hasDotNotation(source))
            throw InvalidArgsException("source path cannot contain any dot notations");

        if (dest[dest.length() - 1] == '/' || dest[dest.length() - 1] == '\\')
            throw InvalidArgsException("dest cannot end with path separator");

        if (hasDotNotation(dest))
            throw InvalidArgsException("dest path cannot contain any dot notations");

        // Nothing to do
        if (source == dest)
            return;

        const auto sourceFound = getEntry(db, source);
        const auto destFound = getEntry(db, dest);

        // Ensure entry consistency: cannot move file on folder and vice-versa
        if (!sourceFound)
            throw InvalidArgsException("source path not found");
        const Entry &sourceEntry = *sourceFound;
        const bool destExists = destFound.has_value();

        // If dest exists
        if (destExists)
        {

            // If source is a folder we cannot move it on anything that exists (only new path)
            if (sourceEntry.type == EntryType::Directory)
            {
                if (destFound->type != EntryType::Directory)
                    throw InvalidArgsException("Cannot move a folder on a file");

                throw InvalidArgsException("Cannot move a directory on another directory");
                // If source is a file we cannot move it on a folder
            }

            if (destFound->type == EntryType::Directory)
                throw InvalidArgsException("Cannot move a file on a directory");
        }

        const fs::path directory = db->rootDirectory();

        // The files moved (old path, new path) and their build keys before the move: their pending markers follow them
        std::vector<std::pair<std::string, std::string>> movedFiles;
        std::vector<std::optional<std::string>> movedKeys;

        Transaction tx(db);

        // If we are moving a file
        if (sourceEntry.type != EntryType::Directory)
        {

            if (destExists)
                deleteEntry(db, dest);

            movedFiles.emplace_back(source, dest);
            movedKeys.push_back(buildKeyAt(db, source));
            replacePath(db, source, dest);

            // If the file extension changed, re-detect the entry type
            const auto sourceExt = io::fromUtf8(source).extension();
            const auto destExt = io::fromUtf8(dest).extension();

            if (sourceExt != destExt)
            {
                const auto newType = fingerprint(directory / io::fromUtf8(dest));
                if (newType != sourceEntry.type)
                {
                    auto typeUpdate = db->query("UPDATE entries SET type = ? WHERE path = ?");
                    typeUpdate->bind(1, static_cast<int>(newType));
                    typeUpdate->bind(2, dest);
                    typeUpdate->execute();
                }
            }
        }
        else
        {

            const auto paths = listFolderPaths(db, source);

            for (const std::string &path : paths)
            {

                auto newPath = dest + std::string(path).substr(source.length(), std::string::npos);

                deleteEntry(db, newPath);
                if (auto key = buildKeyAt(db, path))
                {
                    movedFiles.emplace_back(path, newPath);
                    movedKeys.push_back(std::move(key));
                }
                replacePath(db, path, newPath);
            }

            createMissingFolders(db);
        }

        tx.commit();

        for (size_t i = 0; i < movedFiles.size(); i++)
        {
            try
            {
                movePendingMarker(db, movedFiles[i].first, movedKeys[i], movedFiles[i].second);
            }
            catch (const std::exception &ex)
            {
                LOGW << "Cannot move the pending marker of " << movedFiles[i].first << ": " << ex.what();
            }
        }

        // A file moved to a name a build waits for releases it, as an add does
        releasePendingBuildsAwaiting(db, sourceEntry.type != EntryType::Directory ? std::vector<std::string>{dest}
                                                                                   : listFilePaths(db, dest));
    }

    void recordBuildWarnings(Database *db, const Entry &e, const std::vector<BuildWarning> &warnings)
    {
        // The properties of the row, or nullopt when the path no longer has this content
        const auto read = [&]() -> std::optional<json>
        {
            const auto q = db->query("SELECT properties FROM entries WHERE path = ? AND hash = ?");
            q->bind(1, e.path);
            q->bind(2, e.hash);
            if (!q->fetch())
                return std::nullopt;
            json properties = json::parse(q->getText(0), nullptr, false);
            if (properties.is_discarded())
                properties = json::object();
            return std::optional<json>(std::move(properties));
        };

        // Read first: most builds report no warning and the entry has none to remove, which
        // takes no write lock
        auto properties = read();
        if (!properties || !applyBuildWarnings(*properties, warnings))
            return;

        Transaction tx(db);
        properties = read();
        if (!properties || !applyBuildWarnings(*properties, warnings))
            return;
        const auto update = db->query("UPDATE entries SET properties = ? WHERE path = ? AND hash = ?");
        update->bind(1, properties->dump());
        update->bind(2, e.path);
        update->bind(3, e.hash);
        update->execute();
        tx.commit();
    }

} // namespace ddb
