/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "dbops.h"

#include "build.h"
#include "ddb.h"
#include "status.h"

#include <cstdlib>

#include "entry_types.h"
#include "exceptions.h"
#include "exif.h"
#include "hash.h"
#include "logger.h"
#include "mio.h"

#include "userprofile.h"
#include "utils.h"
#include "version.h"

namespace ddb
{

#define UPDATE_QUERY                                                              \
    "UPDATE entries SET hash=?, type=?, properties=?, mtime=?, size=?, depth=?, " \
    "point_geom=GeomFromText(?, 4326), polygon_geom=GeomFromText(?, 4326) "       \
    "WHERE path=?"

    std::unique_ptr<Database> open(const std::string &directory,
                                   bool traverseUp = false)
    {
        const fs::path dirPath = fs::absolute(directory);
        const fs::path ddbDirPath = dirPath / DDB_FOLDER;
        const fs::path dbasePath = ddbDirPath / "dbase.sqlite";

        if (!exists(dbasePath))
        {
            if (!traverseUp || dirPath.parent_path() == dirPath)
                throw FSException(
                    "Not a valid DroneDB directory, .ddb does not exist. Did you "
                    "run ddb init?");

            return open(dirPath.parent_path().string(), true);
        }

        LOGD << dbasePath.string() + " exists";

        auto db = std::make_unique<Database>();

        db->open(dbasePath.string());

        if (!db->tableExists("entries"))
            throw DBException("Table 'entries' not found (not a valid database: " +
                              dbasePath.string() + ")");

        db->ensureSchemaConsistency();

        return db;
    }

    // Computes a list of paths inside rootDirectory
    // all paths must be subfolders/files within rootDirectory
    // or an exception is thrown
    // If includeDirs is true and the list includes paths to directories that are in
    // paths eg. if path/to/file is in paths, both "path/" and "path/to" are
    // included in the result.
    // ".ddb" files/dirs are always ignored and skipped.
    // If a directory is in the input paths, they are included regardless of
    // includeDirs
    std::vector<fs::path> getIndexPathList(const fs::path &rootDirectory,
                                           const std::vector<std::string> &paths,
                                           bool includeDirs)
    {
        std::vector<fs::path> result;
        std::unordered_map<std::string, bool> directories;

        for (const std::string &p : paths)
        {
            if (p.empty())
                throw FSException("Some paths are empty");
        }

        if (!io::Path(rootDirectory).hasChildren(paths))
        {
            throw FSException("Some paths are not contained within: " +
                              rootDirectory.string() + ". Did you run ddb init?");
        }

        io::Path rootDir = rootDirectory;

        for (fs::path p : paths)
        {
            // fs::directory_options::skip_permission_denied
            if (p.filename() == DDB_FOLDER)
                continue;

            if (fs::is_directory(p))
            {
                try
                {
                    for (auto i = fs::recursive_directory_iterator(p);
                         i != fs::recursive_directory_iterator(); ++i)
                    {
                        fs::path rp = i->path();

                        // Skip .ddb
                        if (rp.filename() == DDB_FOLDER)
                            i.disable_recursion_pending();

                        if (fs::is_directory(rp) && includeDirs)
                        {
                            directories[rp.string()] = true;
                        }
                        else
                        {
                            result.push_back(rp);
                        }

                        if (includeDirs)
                        {
                            while (rp.has_parent_path() &&
                                   rootDir.isParentOf(rp.parent_path()) &&
                                   rp.string() != rp.parent_path().string())
                            {
                                rp = rp.parent_path();
                                directories[rp.string()] = true;
                            }
                        }
                    }
                }
                catch (const fs::filesystem_error &e)
                {
                    throw FSException(e.what());
                }

                directories[p.string()] = true;
                if (includeDirs)
                {
                    while (p.has_parent_path() &&
                           rootDir.isParentOf(p.parent_path()) &&
                           p.string() != p.parent_path().string())
                    {
                        p = p.parent_path();
                        directories[p.string()] = true;
                    }
                }
            }
            else if (fs::exists(p))
            {
                // File
                result.push_back(p);

                if (includeDirs)
                {
                    while (p.has_parent_path() &&
                           rootDir.isParentOf(p.parent_path()) &&
                           p.string() != p.parent_path().string())
                    {
                        p = p.parent_path();
                        directories[p.string()] = true;
                    }
                }
            }
            else
            {
                throw FSException("Path does not exist: " + p.string());
            }
        }

        for (auto [fst, snd] : directories)
        {
            result.emplace_back(fst);
        }

        return result;
    }

    std::vector<fs::path> getPathList(const std::vector<std::string> &paths,
                                      bool includeDirs, int maxDepth, bool includeFiles)
    {
        std::vector<fs::path> result;
        std::unordered_map<std::string, bool> directories;

        for (fs::path p : paths)
        {
            // fs::directory_options::skip_permission_denied
            if (p.filename() == DDB_FOLDER)
                continue;

            try
            {
                if (fs::is_directory(p))
                {
                    for (auto i = fs::recursive_directory_iterator(p);
                         i != fs::recursive_directory_iterator(); ++i)
                    {
                        fs::path rp = i->path();

                        // Ignore system files on Windows
#ifdef WIN32
                        const DWORD attrs =
                            GetFileAttributesW(rp.wstring().c_str());
                        if (attrs & FILE_ATTRIBUTE_HIDDEN ||
                            attrs & FILE_ATTRIBUTE_SYSTEM)
                        {
                            i.disable_recursion_pending();
                            continue;
                        }
#endif

                        // Skip .ddb recursion
                        if (rp.filename() == DDB_FOLDER)
                            i.disable_recursion_pending();

                        // Max depth
                        if (maxDepth > 0 && i.depth() >= (maxDepth - 1))
                            i.disable_recursion_pending();
                        else if (maxDepth == -1)
                            i.disable_recursion_pending();

                        if (fs::is_directory(rp))
                        {
                            if (includeDirs)
                                result.push_back(rp);
                        }
                        else
                        {
                            if (includeFiles)
                                result.push_back(rp);
                        }
                    }
                }
                else if (fs::exists(p) && includeFiles)
                {
                    // File
                    result.push_back(p);
                }
                else
                {
                    throw FSException("Path does not exist: " + p.string());
                }
            }
            catch (const fs::filesystem_error &e)
            {
                throw FSException(e.what());
            }
        }

        return result;
    }

    std::vector<std::string> expandPathList(const std::vector<std::string> &paths,
                                            bool recursive, int maxRecursionDepth)
    {
        std::vector<std::string> result;

        if (!recursive)
        {
            for (auto &p : paths)
            {
                result.push_back(fs::absolute(p).string());
            }
        }
        else
        {
            auto pl = getPathList(paths, true, maxRecursionDepth);
            for (auto &p : pl)
            {
                result.push_back(fs::absolute(p).string());
            }
        }

        return result;
    }

    FileStatus checkUpdate(Entry &e, const fs::path &p, long long dbMtime,
                           const std::string &dbHash)
    {

        if (!exists(p))
            return Deleted;

        const bool folder = fs::is_directory(p);

        if (folder)
            return NotModified;

        // Did it change?
        e.mtime = io::Path(p).getModifiedTime();

        if (e.mtime != dbMtime)
        {
            LOGD << p.string() << " modified time ( " << dbMtime
                 << " ) differs from file value: " << e.mtime;

            e.hash = Hash::fileSHA256(p.string());

            if (dbHash != e.hash)
            {
                LOGD << p.string() << " hash differs (old: " << dbHash
                     << " | new: " << e.hash << ")";
                return Modified;
            }
        }

        return NotModified;
    }

    void doUpdate(Statement *updateQ, const Entry &e)
    {
        // Fields
        updateQ->bind(1, e.hash);
        updateQ->bind(2, e.type);
        updateQ->bind(3, e.properties.dump());
        updateQ->bind(4, static_cast<long long>(e.mtime));
        updateQ->bind(5, static_cast<long long>(e.size));
        updateQ->bind(6, e.depth);
        updateQ->bind(7, e.point_geom.toWkt());
        updateQ->bind(8, e.polygon_geom.toWkt());

        // Where
        updateQ->bind(9, e.path);

        updateQ->execute();
    }

    void listIndex(Database *db, const std::vector<std::string> &paths, std::ostream &output, const std::string &format, bool recursive, int maxRecursionDepth)
    {
        if (format != "json" && format != "text")
            throw InvalidArgsException("Invalid format " + format);

        const fs::path directory = db->rootDirectory();
        std::vector<fs::path> pathList;

        if (paths.empty())
        {
            const auto currentPath = fs::current_path();
            auto root = io::Path(directory);

            // If we are inside ddb folder we can use our current path
            // otherwise we should reset to root folder
            // I.E: \home\tmp\ddb ls -w \home\img

            pathList.emplace_back(root.isParentOf(fs::current_path()) ? io::Path(currentPath).generic() : directory.string());
        }
        else
            pathList = std::vector<fs::path>(paths.begin(), paths.end());

        std::vector<Entry> baseEntries;
        bool expandFolders = recursive;

        for (const fs::path &path : pathList)
        {
            io::Path relPath = io::Path(path).relativeTo(directory);

            auto pathStr = relPath.generic();

            // Let's expand only if we were asked to list a different folder
            expandFolders = expandFolders || pathStr.length() > 0;

            const auto depth = static_cast<int>(count(pathStr.begin(), pathStr.end(), '/'));
            std::vector<Entry> matches = getMatchingEntries(db, relPath.generic(), depth + 1);
            baseEntries.insert(baseEntries.end(), matches.begin(), matches.end());
        }

        // Remove duplicates
        sort(baseEntries.begin(), baseEntries.end(), [](const Entry &l, const Entry &r)
             { return l.path < r.path; });
        baseEntries.erase(unique(baseEntries.begin(), baseEntries.end(), [](const Entry &l, const Entry &r)
                                 { return l.path == r.path; }),
                          baseEntries.end());

        // Sort by type
        sort(baseEntries.begin(), baseEntries.end(), [](const Entry &l, const Entry &r)
             { return l.type < r.type; });

        const bool isSingle = pathList.size() == baseEntries.size();

        std::vector<Entry> outputEntries;

        for (const Entry &entry : baseEntries)
        {
            if (entry.type != Directory)
                outputEntries.emplace_back(Entry(entry));
            else
            {
                if (!isSingle || !expandFolders)
                    outputEntries.emplace_back(Entry(entry));

                if (expandFolders)
                {
                    const auto depth = recursive ? maxRecursionDepth : entry.depth + 2;
                    std::vector<Entry> entries = getMatchingEntries(db, entry.path, depth, true);
                    for (const Entry &e : entries)
                        outputEntries.emplace_back(Entry(e));
                }
            }
        }

        // Sort by path
        std::sort(outputEntries.begin(), outputEntries.end(), [](const Entry &l, const Entry &r)
                  { return l.path < r.path; });

        if (format == "text")
        {
            for (auto &e : outputEntries)
            {
                output << e.path << std::endl;
            }
        }
        else if (format == "json")
        {
            output << "[";
            bool first = true;

            for (auto &e : outputEntries)
            {

                json j;
                e.toJSON(j);
                if (!first)
                    output << ",";
                output << j.dump();

                first = false;
            }

            output << "]";
        }
        else
        {
            throw InvalidArgsException("Unsupported format '" + format + "'");
        }
    }

    void searchIndex(Database *db, const std::string &query, std::ostream &out, const std::string &format)
    {
        auto entries = getMatchingEntries(db, query, 0, false);
        std::sort(entries.begin(), entries.end(), [](const Entry &l, const Entry &r)
                  { return l.path < r.path; });

        if (format == "text")
        {
            for (auto &e : entries)
                out << e.path << std::endl;
        }
        else if (format == "json")
        {
            out << "[";
            bool first = true;
            for (auto &e : entries)
            {
                json j;
                e.toJSON(j);
                if (!first)
                    out << ",";
                out << j.dump();
                first = false;
            }
            out << "]";
        }
    }

    void addToIndex(Database *db, const std::vector<std::string> &paths,
                    AddCallback callback)
    {
        if (paths.empty())
            return; // Nothing to do
        const fs::path directory = db->rootDirectory();
        auto pathList = getIndexPathList(directory, paths, true);

        auto q = db->query("SELECT mtime,hash FROM entries WHERE path=?");
        auto insertQ = db->query(
            "INSERT INTO entries (path, hash, type, properties, mtime, size, depth, "
            "point_geom, polygon_geom) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, GeomFromText(?, 4326), GeomFromText(?, "
            "4326))");
        const auto updateQ = db->query(UPDATE_QUERY);
        db->exec("BEGIN EXCLUSIVE TRANSACTION");

        for (auto &p : pathList)
        {
            io::Path relPath = io::Path(p).relativeTo(directory);

            if (p.has_filename())
            {
                const auto fileName = p.filename().generic_string();
                if (fileName.find('\\') != std::string::npos)
                {

                    LOGD << "Skipping '" << p << "'";

                    // Skip file
                    continue;
                }
            }

            q->bind(1, relPath.generic());

            bool update = false;
            bool add = false;
            Entry e;

            if (q->fetch())
            {

                const auto status = checkUpdate(e, p, q->getInt64(0), q->getText(1));

                // Entry exist, update if necessary
                update = status != FileStatus::NotModified;
            }
            else
            {
                // Brand new, add
                add = true;
            }

            if (add || update)
            {
                parseEntry(p, directory, e, true);

                if (add)
                {
                    insertQ->bind(1, e.path);
                    insertQ->bind(2, e.hash);
                    insertQ->bind(3, e.type);
                    insertQ->bind(4, e.properties.dump());
                    insertQ->bind(5, static_cast<long long>(e.mtime));
                    insertQ->bind(6, static_cast<long long>(e.size));
                    insertQ->bind(7, e.depth);
                    insertQ->bind(8, e.point_geom.toWkt());
                    insertQ->bind(9, e.polygon_geom.toWkt());

                    insertQ->execute();
                }
                else
                {
                    doUpdate(updateQ.get(), e);
                }

                if (callback != nullptr)
                    if (!callback(e, !add))
                        return; // cancel
            }

            q->reset();
        }

        db->exec("COMMIT");
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

        auto pathList = std::vector<fs::path>(paths.begin(), paths.end());

        for (auto &p : pathList)
        {
            LOGD << "Deleting path: " << p;

            auto relPath = io::Path(p).relativeTo(directory);

            LOGD << "Rel path: " << relPath.generic();

            auto entryMatches = getMatchingEntries(db, relPath.generic());

            int tot = 0;

            for (auto &e : entryMatches)
            {
                auto cnt = deleteFromIndex(db, e.path, false, callback);

                if (e.type == Directory)
                    cnt += deleteFromIndex(db, e.path, true, callback);

                // if (!cnt)
                //     std::cout << "No matching entries" << std::endl;

                tot += cnt;
            }

            if (!tot)
                throw FSException("No matching entries");
        }
    }

    std::string sanitize_query_param(const std::string &str)
    {
        std::string res(str);

        // TAKES INTO ACCOUNT PATHS THAT CONTAINS EVERY SORT OF STUFF
        utils::stringReplace(res, "/", "//");
        utils::stringReplace(res, "%", "/%");
        // utils::stringReplace(res, "_", "/_");
        utils::stringReplace(res, "*", "%");

        return res;
    }

    void checkDeleteBuild(Database *db, const std::string &hash)
    {
        if (!hash.empty())
        {
            const auto buildFolder = db->buildDirectory() / hash;

            if (fs::exists(buildFolder))
            {
                LOGD << "Removing " << (buildFolder).string();
                io::assureIsRemoved(buildFolder);
            }
        }
    }

    void checkDeleteMeta(Database *db, const std::string &path)
    {
        if (!path.empty())
        {
            auto q = db->query("DELETE FROM entries_meta WHERE path = ?");
            q->bind(1, path);
            q->execute();
        }
    }

    int deleteFromIndex(Database *db, const std::string &query, bool isFolder, RemoveCallback callback)
    {

        LOGD << "Query: " << query;

        auto str = sanitize_query_param(query);

        LOGD << "Sanitized: " << str;

        if (isFolder)
        {
            str += "//%";
            LOGD << "Folder: " << str;
        }

        db->exec("BEGIN EXCLUSIVE TRANSACTION");

        auto q = db->query(
            "SELECT path, hash FROM entries WHERE path LIKE ? ESCAPE '/'");

        q->bind(1, str);

        int count = 0;

        while (q->fetch())
        {

            const auto path = q->getText(0);
            const auto hash = q->getText(1);

            // Check for build folders to be removed
            checkDeleteBuild(db, hash);

            // Check for meta info to be removed
            checkDeleteMeta(db, path);

            if (callback != nullptr)
                callback(path);

            count++;
        }

        q->reset();

        if (count > 0)
        {
            q = db->query("DELETE FROM entries WHERE path LIKE ? ESCAPE '/'");

            q->bind(1, str);
            q->execute();

            q->reset();
        }

        db->exec("COMMIT");

        return count;
    }

    std::vector<Entry> getMatchingEntries(Database *db, const fs::path &path,
                                          int maxRecursionDepth, bool isFolder)
    {
        // 0 is ALL_DEPTHS
        if (maxRecursionDepth < 0)
            throw FSException("Max recursion depth cannot be negative");

        const auto query = path.string();

        LOGD << "Query: " << query;

        auto sanitized = sanitize_query_param(query);

        if (sanitized.length() == 0)
            sanitized = "%";

        LOGD << "Sanitized: " << sanitized;

        if (isFolder)
        {
            sanitized += "//%";

            LOGD << "Folder: " << sanitized;
        }

        std::string sql = R"<<<(
        SELECT e.path, e.hash, e.type, e.properties, e.mtime, e.size, e.depth,
        json_extract(AsGeoJSON(e.point_geom), '$.coordinates'), json_extract(AsGeoJSON(e.polygon_geom), '$.coordinates'),
        CASE
            WHEN em.id IS NULL THEN NULL
            WHEN em.id IS NOT NULL THEN (
                SELECT json_group_object(key, meta)
                FROM (
                    SELECT key, CASE WHEN substr(key, -1, 1) = 's'
                                    THEN json_group_array(json_object('id', emi.id, 'data', json(emi.data), 'mtime', emi.mtime))
                                    ELSE json_object('id', emi.id, 'data', json(emi.data), 'mtime', emi.mtime)
                                END AS meta
                    FROM entries_meta emi
                    WHERE path = e.path
                    GROUP BY key
                )
            )
        END AS meta
        FROM entries e
        LEFT JOIN entries_meta em
        ON e.path = em.path
        WHERE
        e.path LIKE ? ESCAPE '/'
    )<<<";

        if (maxRecursionDepth > 0)
            sql += " AND e.depth <= " + std::to_string(maxRecursionDepth - 1);

        auto q = db->query(sql);

        std::vector<Entry> entries;

        q->bind(1, sanitized);

        while (q->fetch())
        {
            Entry e(q->getText(0), q->getText(1), q->getInt(2), q->getText(3),
                    q->getInt64(4), q->getInt64(5), q->getInt(6),
                    q->getText(7), q->getText(8),
                    q->getText(9));
            entries.push_back(e);
        }

        q->reset();

        return entries;
    }

    void syncIndex(Database *db)
    {
        const fs::path directory = db->rootDirectory();

        auto q = db->query("SELECT path,mtime,hash FROM entries");
        auto deleteQ = db->query("DELETE FROM entries WHERE path = ?");
        const auto updateQ = db->query(UPDATE_QUERY);

        db->exec("BEGIN EXCLUSIVE TRANSACTION");

        while (q->fetch())
        {
            io::Path relPath = fs::path(q->getText(0));
            fs::path p = directory / relPath.get();
            Entry e;
            const auto mtime = q->getInt64(1);
            const auto hash = q->getText(2);
            const auto status = checkUpdate(e, p, mtime, hash);

            switch (status)
            {

            case Deleted:
                // Removed
                deleteQ->bind(1, relPath.generic());
                deleteQ->execute();
                checkDeleteBuild(db, hash);
                checkDeleteMeta(db, relPath.generic());
                std::cout << "D\t" << relPath.generic() << std::endl;
                break;

            case Modified:

                parseEntry(p, directory, e, true);
                doUpdate(updateQ.get(), e);
                std::cout << "U\t" << e.path << std::endl;
                break;

            default:; // Do nothing
            }
        }

        db->exec("COMMIT");
    }

    // Sets the modified times of files in the filesystem
    // to match that of the database. Optionally,
    // if a whitelist of files is specified, limits the scope of the synchronization
    // to those files only. An empty set of files indicates to update all files.
    void syncLocalMTimes(Database *db, const std::vector<std::string> &files)
    {
        const fs::path directory = db->rootDirectory();
        std::string sql = "SELECT path,mtime FROM entries WHERE (type != ? AND type != ?)";
        if (files.size() > 0)
        {
            sql += " AND path in (";
            for (unsigned long i = 0; i < files.size() - 1; i++)
                sql += "?,";
            sql += "?)";
        }

        auto q = db->query(sql);
        q->bind(1, EntryType::Directory);
        q->bind(2, EntryType::DroneDB);
        for (unsigned long i = 0; i < files.size(); i++)
        {
            q->bind(3 + i, files[i]);
        }

        while (q->fetch())
        {
            io::Path fullPath = directory / fs::path(q->getText(0));
            if (fullPath.setModifiedTime(static_cast<time_t>(q->getInt64(1))))
            {
                LOGD << "Updated mtime for " << fullPath.string();
            }
        }
    }

    std::string initIndex(const std::string &directory, bool fromScratch)
    {
        const fs::path dirPath = directory;
        if (!exists(dirPath))
            throw FSException("Invalid directory: " + dirPath.string() +
                              " (does not exist)");

        auto ddbDirPath = dirPath / DDB_FOLDER;
        if (std::string(directory) == ".")
            ddbDirPath = DDB_FOLDER; // Nicer to the eye
        const auto dbasePath = ddbDirPath / "dbase.sqlite";

        LOGD << "Checking if .ddb directory exists...";
        if (exists(ddbDirPath))
        {
            throw FSException("Cannot initialize database: " + ddbDirPath.string() +
                              " already exists");
        }

        if (create_directory(ddbDirPath))
        {
            LOGD << ddbDirPath.string() + " created";
        }
        else
        {
            throw FSException("Cannot create directory: " + ddbDirPath.string() +
                              ". Check that you have the proper permissions?");
        }

        LOGD << "Checking if database exists...";
        if (exists(dbasePath))
        {
            throw FSException(ddbDirPath.string() + " already exists");
        }

        if (!fromScratch)
        {
            // "Fast" init by copying the pre-built empty database index
            // this prevents the slow table generation process
            const fs::path emptyDbPath = UserProfile::get()->getTemplatesDir() /
                                         ("empty-dbase-" APP_REVISION ".sqlite");

            // Need to create?
            if (!exists(emptyDbPath))
            {
                LOGD << "Creating " << emptyDbPath.string();

                // Create database
                auto db = std::make_unique<Database>();
                db->open(emptyDbPath.string());
                db->createTables();
                db->close();
            }

            if (exists(emptyDbPath))
            {
                // Copy
                try
                {
                    copy(emptyDbPath, dbasePath);
                }
                catch (fs::filesystem_error &e)
                {
                    throw FSException(e.what());
                }

                LOGD << "Copied " << emptyDbPath.string() << " to "
                     << dbasePath.string();
            }
            else
            {
                // For some reason it's missing, generate from scratch
                LOGD << "Cannot find empty-dbase.sqlite in data path, strange! "
                        "Building from scratch instead";
                fromScratch = true;
            }
        }

        if (fromScratch)
        {
            LOGD << "Creating " << dbasePath.string();

            // Create database
            auto db = std::make_unique<Database>();
            db->open(dbasePath.string());
            db->createTables();
            db->close();
        }

        return ddbDirPath.string();
    }

    void deleteEntry(Database *db, const std::string &path)
    {

        auto f = db->query("DELETE FROM entries WHERE path = ?");
        f->bind(1, path);
        f->execute();

        checkDeleteMeta(db, path);
    }

#define FOLDER_CONSISTENCY_QUERY "SELECT B.folder FROM ( \
    SELECT A.path, TRIM(A.folder, '/') AS folder FROM ( \
        SELECT path, replace(path, replace(path, rtrim(path, replace(path, '/', '')), ''), '') AS folder FROM entries WHERE type != 1) AS A \
        WHERE length(A.folder) > 0) AS B WHERE folder NOT IN (SELECT path FROM entries WHERE type = 1)"

#define CREATE_FOLDER_QUERY "INSERT INTO entries (path, type, properties, mtime, size, depth) VALUES (?, 1, 'null', ?, 0, ?)"

    void addFolder(Database *db, const std::string path, const time_t mtime)
    {
        const auto q = db->query(CREATE_FOLDER_QUERY);
        q->bind(1, path);
        q->bind(2, static_cast<long long>(mtime));
        q->bind(3, ddb::io::Path(path).depth());
        q->execute();
    }

    void createMissingFolders(Database *db)
    {

        const auto q = db->query(FOLDER_CONSISTENCY_QUERY);

        while (q->fetch())
        {

            const auto folder = q->getText(0);

            LOGD << "Creating missing folder '" << folder << "'";

            addFolder(db, folder, time(NULL));
        }
    }

    bool pathExists(Database *db, const std::string &path)
    {
        auto q = db->query("SELECT COUNT(path) FROM entries WHERE path = ?");
        q->bind(1, path);
        q->fetch();
        return q->getInt(0) > 0;
    }

    bool getEntry(Database *db, const std::string &path, Entry &entry)
    {
        auto q = db->query("SELECT path, hash, type, properties, mtime, size, depth, "
                           "json_extract(AsGeoJSON(point_geom), '$.coordinates'), json_extract(AsGeoJSON(polygon_geom), '$.coordinates') FROM entries WHERE path = ? LIMIT 1");

        q->bind(1, path);

        if (!q->fetch())
            return false;

        entry.parseFields(q->getText(0), q->getText(1), q->getInt(2), q->getText(3),
                          q->getInt64(4), q->getInt64(5), q->getInt(6),
                          q->getText(7), q->getText(8));
        return true;
    }

    std::vector<std::string> listFolderPaths(Database *db, const std::string &path)
    {

        std::vector<std::string> res;

        auto q = db->query("SELECT path FROM entries WHERE path LIKE ? OR path = ?");

        q->bind(1, path + "/%");
        q->bind(2, path);

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

        const auto depth = io::Path(dest).depth();

        auto update = db->query("UPDATE entries SET path = ?, depth = ? WHERE path = ?");
        update->bind(1, dest);
        update->bind(2, depth);
        update->bind(3, source);
        update->execute();

        // Move meta
        auto mq = db->query("UPDATE entries_meta SET path = ? WHERE path = ?");
        mq->bind(1, dest);
        mq->bind(2, source);
        mq->execute();
    }

    void moveEntry(Database *db, const std::string &source, const std::string &dest)
    {

        if (source[source.length() - 1] == '/' || source[source.length() - 1] == '\\')
            throw InvalidArgsException("source cannot end with path separator");

        if (utils::hasDotNotation(source))
            throw InvalidArgsException("source path cannot contain any dot notations");

        if (dest[dest.length() - 1] == '/' || dest[dest.length() - 1] == '\\')
            throw InvalidArgsException("dest cannot end with path separator");

        if (utils::hasDotNotation(dest))
            throw InvalidArgsException("dest path cannot contain any dot notations");

        // Nothing to do
        if (source == dest)
            return;

        Entry sourceEntry, destEntry;
        bool sourceExists = getEntry(db, source, sourceEntry);
        bool destExists = getEntry(db, dest, destEntry);

        // Ensure entry consistency: cannot move file on folder and vice-versa
        if (!sourceExists)
            throw InvalidArgsException("source path not found");

        // If dest exists
        if (destExists)
        {

            // If source is a folder we cannot move it on anything that exists (only new path)
            if (sourceEntry.type == Directory)
            {
                if (destEntry.type != Directory)
                    throw InvalidArgsException("Cannot move a folder on a file");

                throw InvalidArgsException("Cannot move a directory on another directory");
                // If source is a file we cannot move it on a folder
            }

            if (destEntry.type == Directory)
                throw InvalidArgsException("Cannot move a file on a directory");
        }

        const fs::path directory = db->rootDirectory();

        db->exec("BEGIN EXCLUSIVE TRANSACTION");

        // If we are moving a file
        if (sourceEntry.type != Directory)
        {

            if (destExists)
                deleteEntry(db, dest);

            replacePath(db, source, dest);
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

        db->exec("COMMIT");
    }

} // namespace ddb
