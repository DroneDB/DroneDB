/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// Changing the index: removal, sync with the filesystem, rescan, and moving entries.
#include "dbops.h"
#include "index_internal.h"

#include "build.h"
#include "entry_filters.h"
#include "entry_path.h"
#include "exceptions.h"
#include "index_sql.h"
#include "logger.h"
#include "mio.h"
#include "parallel.h"
#include "raster_cache.h"
#include "retrypolicy.h"
#include "status.h"
#include "transaction.h"

#include <ctime>
#include <map>

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

    // Deletes the entries matching a predicate, and their metadata, inside the caller's
    // transaction. The statements are prepared once for one predicate sql and bound again
    // for each run, so removing many paths does not prepare three statements per path.
    class RowDeleter
    {
        std::unique_ptr<Statement> metaDelete_, select_, delete_;

    public:
        RowDeleter(Database *db, const std::string &predicateSql)
            : metaDelete_(db->query("DELETE FROM entries_meta WHERE " + predicateSql)),
              select_(db->query("SELECT path, hash FROM entries WHERE " + predicateSql)),
              delete_(db->query("DELETE FROM entries WHERE " + predicateSql))
        {
        }

        // `predicate` has the sql given to the constructor. Appends the deleted paths and
        // their (non-empty) hashes; returns the count.
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
                    deletedHashes.push_back(hash);
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

    // Removes the build folders of deleted entries, on the I/O threads (ioThreads), outside any
    // transaction.
    void deleteBuildFolders(Database *db, const std::vector<std::string> &hashes)
    {
        if (hashes.empty())
            return;

        const auto buildDir = db->buildDirectory();
        parallel::forEachIndexIo(hashes.size(), [&](size_t i)
                                 {
            try
            {
                removeBuildFolder(buildDir / hashes[i]);
            }
            catch (const std::exception &e)
            {
                LOGD << "Warning: Failed to delete build folder: " << e.what();
            } });
    }

    } // namespace

    void checkDeleteBuild(Database *db, const std::string &hash)
    {
        if (!hash.empty())
            removeBuildFolder(db->buildDirectory() / hash);
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

        auto q = db->query("SELECT path,mtime,hash FROM entries");
        auto deleteQ = db->query("DELETE FROM entries WHERE path = ?");
        const auto updateQ = db->query(kUpdateEntrySql);

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
            double fileMtime = 0;
            const auto status = checkUpdate(e, p, mtime, hash, &fileMtime);

            switch (status)
            {

            case Deleted:
                // Removed
                deleteQ->bind(1, relPath.generic());
                deleteQ->execute();
                checkDeleteBuild(db, hash);
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

    void rescanIndex(Database *db, const std::vector<EntryType> &types, const std::vector<std::string> &extensions,
                     bool stopOnError, RescanCallback callback)
    {
        const fs::path directory = db->rootDirectory();

        // Always exclude directories from rescan because they do not contain file metadata
        // that can be re-parsed. This check is defensive and prevents potential issues
        // even if validation fails at higher layers (C API, CLI).
        SqlConditions conditions;
        conditions.add(typeCondition("type", "!=", EntryType::Directory));
        if (!types.empty())
            conditions.addEntryTypes("type", types);

        auto q = db->query("SELECT path FROM entries" + conditions.whereClause());
        conditions.bind(*q);

        const auto updateQ = db->query(kUpdateEntrySql);

        // Deliberately one IMMEDIATE transaction for the whole rescan (design choice, kept):
        // the entries table is always left in a consistent, fully re-parsed state, even for
        // consumers reading between statements. The cost is that re-scan holds the sole SQLite
        // writer lock for its entire duration; concurrent addToIndexEx callers see busy and
        // retry/jitter per RetryPolicy.
        Transaction tx(db);

        while (q->fetch())
        {
            const std::string path = q->getText(0);
            if (!extensions.empty() && !filters::hasAnyExtension(path, extensions))
                continue;

            io::Path relPath = io::fromUtf8(path);
            fs::path fullPath = directory / relPath.get();

            // Check if file still exists (outside try-catch to handle stopOnError correctly)
            if (!fs::exists(fullPath))
            {
                std::string error = "File not found: " + io::toUtf8(fullPath);
                LOGD << error;

                if (callback != nullptr)
                {
                    Entry e;
                    e.path = relPath.generic();
                    // tx's destructor rolls back on this early return (I4/C8).
                    if (!callback(e, false, error))
                        return;
                }

                if (stopOnError)
                    throw FSException(error); // tx's destructor rolls back

                continue;
            }

            try
            {
                Entry e;
                parseEntry(fullPath, directory, e, true);
                doUpdate(updateQ.get(), e);

                if (callback != nullptr)
                {
                    if (!callback(e, true, ""))
                        return; // tx's destructor rolls back
                }
            }
            catch (const std::exception &ex)
            {
                std::string error = ex.what();
                LOGD << "Error processing " << fullPath << ": " << error;

                if (callback != nullptr)
                {
                    Entry e;
                    e.path = relPath.generic();
                    if (!callback(e, false, error))
                        return; // tx's destructor rolls back
                }

                if (stopOnError)
                    throw; // tx's destructor rolls back
            }
        }

        tx.commit();
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

        Transaction tx(db);

        // If we are moving a file
        if (sourceEntry.type != EntryType::Directory)
        {

            if (destExists)
                deleteEntry(db, dest);

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
                replacePath(db, path, newPath);
            }

            createMissingFolders(db);
        }

        tx.commit();
    }

} // namespace ddb
