/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "dbops.h"

#include "build.h"
#include "entry_handler.h"
#include "cad.h"
#include "databasepool.h"
#include "raster_cache.h"
#include "ddb.h"
#include "status.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#include "entry_path.h"
#include "entry_types.h"
#include "exceptions.h"
#include "exif.h"
#include "hash.h"
#include "logger.h"
#include "mio.h"
#include "parallel.h"

#include "userprofile.h"
#include "utf8.h"
#include "utils.h"
#include "version.h"
#include "constants.h"
#include "transaction.h"
#include "retrypolicy.h"

#include <glob.hpp>

namespace ddb
{

namespace
{
    // Removes a closed SQLite database and the -wal/-shm files SQLite keeps next to it
    // (Database::afterOpen sets SQLITE_FCNTL_PERSIST_WAL). Best effort.
    void removeDatabaseFiles(const fs::path &file, bool keepMainFile = false)
    {
        std::error_code ignored;
        if (!keepMainFile)
            fs::remove(file, ignored);
        fs::remove(file.string() + "-wal", ignored);
        fs::remove(file.string() + "-shm", ignored);
    }

    // A database file under construction: removed with its -wal/-shm files when the owner
    // goes out of scope, unless it was published (renamed into place) first
    class TemporaryDatabaseFile
    {
        fs::path file_;
        bool published_ = false;

      public:
        explicit TemporaryDatabaseFile(fs::path file) : file_(std::move(file)) {}
        ~TemporaryDatabaseFile()
        {
            if (!published_)
                removeDatabaseFiles(file_);
        }
        TemporaryDatabaseFile(const TemporaryDatabaseFile &) = delete;
        TemporaryDatabaseFile &operator=(const TemporaryDatabaseFile &) = delete;

        const fs::path &path() const { return file_; }

        // Renames the closed database to target (its -wal/-shm files are removed). False when
        // the rename fails: the file stays owned and is removed on scope exit.
        bool publish(const fs::path &target, std::error_code &error)
        {
            // Closed by its only connection: checkpointed, the WAL is empty
            removeDatabaseFiles(file_, true);
            fs::rename(file_, target, error);
            published_ = !error;
            return published_;
        }
    };

    // Creates the empty index template that initIndex() copies, unless it exists.
    //
    // Threads and processes sharing a profile can get here together on a fresh profile. Each
    // builds the template in a temporary file of its own, closes it and renames it to
    // templatePath: the rename is atomic, so templatePath only ever names a complete
    // database. The last rename wins on POSIX (all candidates are identical); where the
    // rename does not replace an existing file (Windows), the caller that loses discards
    // its copy.
    void ensureEmptyIndexTemplate(const fs::path &templatePath)
    {
        if (exists(templatePath))
            return;

        LOGD << "Creating " << templatePath.string();
        TemporaryDatabaseFile temporary(templatePath.string() + ".tmp-" +
                                        utils::generateRandomString(16));
        {
            Database db;
            db.open(temporary.path().string());
            db.createTables();
            db.close();
        }

        std::error_code e;
        if (!temporary.publish(templatePath, e))
        {
            if (!exists(templatePath))
                throw FSException("Cannot publish " + templatePath.string() + " (" +
                                  e.message() + ")", e);
            LOGD << templatePath.string() << " was published by another process";
        }
    }
}

#define UPDATE_QUERY                                                              \
    "UPDATE entries SET hash=?, type=?, properties=?, mtime=?, size=?, depth=?, " \
    "point_geom=GeomFromText(?, 4326), polygon_geom=GeomFromText(?, 4326) "       \
    "WHERE path=?"

    std::unique_ptr<Database> open(const std::string &directory,
                                   bool traverseUp = false)
    {
        const fs::path dirPath = fs::absolute(directory);
        const fs::path ddbDirPath = dirPath / DDB_FOLDER;
        const fs::path dbasePath = ddbDirPath / DDB_DATABASE_FILE;

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

        // An idle connection to this index skips the open, the SpatiaLite setup, the
        // schema parse and the checks below (it passed them when it was opened)
        auto &pool = DatabasePool::instance();
        if (pool.acquire(dbasePath, *db))
            return db;

        db->open(dbasePath.string());

        if (!db->tableExists("entries"))
            throw DBException("Table 'entries' not found (not a valid database: " +
                              dbasePath.string() + ")");

        db->ensureSchemaConsistency();
        db->removeRootEntry();
        pool.adopt(dbasePath, *db);

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
                                           bool includeDirs, bool recursive)
    {
        std::vector<fs::path> result;
        std::unordered_map<std::string, bool> directories;

        // A file can be listed explicitly and found again under a listed folder
        std::unordered_set<std::string> files;
        const auto addFile = [&result, &files](const fs::path &f)
        {
            if (files.insert(f.string()).second)
                result.push_back(f);
        };

        io::Path rootDir = rootDirectory;

        // Records the folders between `path` and the root. Stops at a folder already
        // recorded: its own parents were recorded with it.
        const auto addParents = [&rootDir, &directories](fs::path path)
        {
            while (path.has_parent_path() && path != path.parent_path())
            {
                path = path.parent_path();
                if (directories.count(path.string()) > 0 || !rootDir.isParentOf(path))
                    break;
                directories[path.string()] = true;
            }
        };

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

        for (fs::path p : paths)
        {
            if (isReservedSegment(p.filename()))
                continue;

            if (fs::is_directory(p))
            {
                if (recursive)
                {
                    try
                    {
                        for (auto i = fs::recursive_directory_iterator(p,
                             fs::directory_options::skip_permission_denied);
                             i != fs::recursive_directory_iterator(); ++i)
                        {
                            fs::path rp = i->path();

                            // Skip .ddb (do not recurse into it and do not record
                            // it as an indexable directory entry).
                            if (isReservedSegment(rp.filename())) {
                                i.disable_recursion_pending();
                                continue;
                            }

                            // The iterator knows the type from the directory listing: no stat
                            if (i->is_directory() && includeDirs)
                            {
                                directories[rp.string()] = true;
                            }
                            else
                            {
                                addFile(rp);
                            }

                            if (includeDirs)
                                addParents(rp);
                        }
                    }
                    catch (const fs::filesystem_error &e)
                    {
                        throw FSException(e.what());
                    }
                }

                // The dataset root is not an entry (`ddb add .` used to index it as "")
                if (rootDir.isParentOf(p))
                    directories[p.string()] = true;
                if (includeDirs)
                    addParents(p);
            }
            else if (fs::exists(p))
            {
                // File
                addFile(p);

                if (includeDirs)
                    addParents(p);
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
            if (isReservedSegment(p.filename()))
                continue;

            try
            {
                if (fs::is_directory(p))
                {
                    for (auto i = fs::recursive_directory_iterator(p,
                         fs::directory_options::skip_permission_denied);
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

                        // Skip .ddb recursion (and don't record it as an entry)
                        if (isReservedSegment(rp.filename())) {
                            i.disable_recursion_pending();
                            continue;
                        }

                        // Max depth
                        if (maxDepth > 0 && i.depth() >= (maxDepth - 1))
                            i.disable_recursion_pending();
                        else if (maxDepth == -1)
                            i.disable_recursion_pending();

                        if (i->is_directory())
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

    bool containsDdbSegment(const fs::path &p)
    {
        for (const auto &part : p)
        {
            if (isReservedSegment(part))
                return true;
        }
        return false;
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

    namespace
    {
        // Returns true if the pattern contains shell glob meta-characters.
        bool isGlobPattern(const std::string &p)
        {
            return p.find_first_of("*?[") != std::string::npos;
        }
    } // namespace

    std::vector<std::string> expandGlobPatterns(
        const std::vector<std::string> &patterns)
    {
        std::vector<std::string> result;
        std::unordered_set<std::string> seen;
        size_t totalMatches = 0;

        const auto pushUnique = [&](const fs::path &p) {
            if (containsDdbSegment(p))
                return;
            std::string abs = fs::absolute(p).string();
            if (seen.insert(abs).second)
                result.push_back(std::move(abs));
        };

        for (const std::string &pattern : patterns)
        {
            if (pattern.empty())
                throw InvalidArgsException("Empty pattern in input list");

            if (isGlobPattern(pattern))
            {
                // `**` triggers recursive globbing; everything else is single-level.
                const bool recursive =
                    pattern.find("**") != std::string::npos;
                std::vector<fs::path> matches;
                try
                {
                    matches = recursive ? glob::rglob(pattern)
                                        : glob::glob(pattern);

                    // p-ranav's `**` requires at least one directory segment,
                    // so `**/<sub>` would skip files at the cwd. To match the
                    // common shell expectation that `**` matches "zero or more
                    // directories", also run the non-recursive variant with
                    // the leading `**/` stripped and merge the results.
                    if (recursive && pattern.rfind("**/", 0) == 0)
                    {
                        const std::string topLevel = pattern.substr(3);
                        if (!topLevel.empty())
                        {
                            auto extra = glob::glob(topLevel);
                            matches.insert(matches.end(),
                                           std::make_move_iterator(extra.begin()),
                                           std::make_move_iterator(extra.end()));
                        }
                    }
                }
                catch (const std::exception &e)
                {
                    LOGD << "Glob error for pattern '" << pattern
                         << "': " << e.what();
                }

                size_t patternMatches = 0;
                for (auto &m : matches)
                {
                    if (containsDdbSegment(m))
                        continue;
                    pushUnique(m);
                    ++patternMatches;
                }

                if (patternMatches == 0)
                {
                    LOGV << "Pattern '" << pattern
                         << "' did not match any files";
                }
                totalMatches += patternMatches;
            }
            else
            {
                fs::path p(pattern);
                if (!fs::exists(p))
                    throw FSException("Path does not exist: " + pattern);

                if (fs::is_directory(p))
                {
                    // Bare directories: expand recursively (preserves the
                    // common `ddb add .` workflow). Hint at glob equivalent.
                    LOGV << "Expanding directory '" << pattern
                         << "' recursively (use '" << pattern
                         << "/**/*' for explicit globbing).";

                    const size_t before = result.size();
                    pushUnique(p);
                    for (auto it = fs::recursive_directory_iterator(p,
                         fs::directory_options::skip_permission_denied);
                         it != fs::recursive_directory_iterator(); ++it)
                    {
                        if (isReservedSegment(it->path().filename())) {
                            it.disable_recursion_pending();
                            continue;
                        }
                        pushUnique(it->path());
                    }
                    // Only count this directory as a match if at least one
                    // entry was actually added (e.g. the directory wasn't
                    // skipped for being inside a `.ddb` folder).
                    if (result.size() > before)
                        ++totalMatches;
                }
                else
                {
                    const size_t before = result.size();
                    pushUnique(p);
                    if (result.size() > before)
                        ++totalMatches;
                }
            }
        }

        if (totalMatches == 0 && !patterns.empty())
            throw FSException(
                "No files matched any of the provided patterns");

        return result;
    }

    FileStatus checkUpdate(Entry &e, const fs::path &p, double dbMtime,
                           const std::string &dbHash, double *fileMtime)
    {

        if (!exists(p))
            return Deleted;

        const bool folder = fs::is_directory(p);

        if (folder)
            return NotModified;

        // Did it change? Sub-second times catch an edit made in the same second as the
        // previous add (same size, same whole-second time)
        const double mtime = io::Path(p).getModifiedTimePrecise();
        e.mtime = static_cast<time_t>(std::floor(mtime));
        if (fileMtime != nullptr)
            *fileMtime = mtime;

        if (!io::sameModifiedTime(dbMtime, mtime))
        {
            LOGD << p.string() << " modified time ( " << std::fixed << dbMtime
                 << " ) differs from file value: " << mtime;

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
        doUpdate(updateQ, e, static_cast<double>(e.mtime));
    }

    void doUpdate(Statement *updateQ, const Entry &e, double mtime)
    {
        // Fields (a whole-number mtime is stored as an integer, like before)
        updateQ->bind(1, e.hash);
        updateQ->bind(2, e.type);
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

        writeEntries(output, outputEntries, format);
    }

    void searchIndex(Database *db, const std::string &query, std::ostream &out, const std::string &format)
    {
        auto entries = getMatchingEntries(db, query, 0, false);
        std::sort(entries.begin(), entries.end(), [](const Entry &l, const Entry &r)
                  { return l.path < r.path; });

        writeEntries(out, entries, format);
    }

    void writeEntries(std::ostream &out, const std::vector<Entry> &entries, const std::string &format,
                      const EntryJsonDecorator &decorate)
    {
        if (format == "text")
        {
            for (const auto &e : entries)
                out << e.path << std::endl;
        }
        else if (format == "json")
        {
            out << "[";
            bool first = true;
            for (const auto &e : entries)
            {
                json j;
                e.toJSON(j);
                if (decorate)
                    decorate(e, j);
                if (!first)
                    out << ",";
                out << j.dump();
                first = false;
            }
            out << "]";
        }
        else
        {
            throw InvalidArgsException("Unsupported format '" + format + "'");
        }
    }

    namespace {

    // Relative path -> the SHA-256 the caller already knows (and the size/mtime it saw, if any)
    using KnownHashes = std::unordered_map<std::string, KnownFile>;

    KnownHashes toKnownHashes(const std::vector<KnownFile> &files, const fs::path &directory) {
        KnownHashes known;
        for (const auto &f : files)
            if (!f.hash.empty())
                known[io::Path(f.path).relativeTo(directory).generic()] = f;
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
    // no transaction held so hashing/EXIF/GDAL/PDAL cost never blocks other writers (I1).
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

            if (p.has_filename()) {
                const auto fileName = p.filename().generic_string();
                if (std::any_of(fileName.begin(), fileName.end(), isEntryPathSeparator)) {
                    LOGD << "Skipping '" << p << "'";
                    continue; // Skip file
                }
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

    // Threads that hash and parse files while indexing (DDB_INDEX_THREADS: N, 0 = every
    // core, default min(8, cores); 1 = serial)
    size_t indexThreads() {
        const int threads = parallel::threadsOption("DDB_INDEX_THREADS", 8);
        return threads == 0 ? parallel::hardwareThreads() : static_cast<size_t>(threads);
    }

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
    // write lock (C1/I1). Items are independent, so they run on indexThreads() threads; the
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
    std::vector<ComputedAddItem> computeAddEntries(const std::vector<PlannedAddItem> &planned,
                                                    const fs::path &directory, bool stopOnError,
                                                    std::vector<AddItemError> &errors,
                                                    std::vector<std::string> &unchanged,
                                                    const KnownHashes *knownHashes = nullptr) {
        std::vector<ComputeOutcome> outcomes(planned.size());
        parallel::forEachIndex(planned.size(), indexThreads(), [&](size_t i) {
            outcomes[i] = computeAddEntry(planned[i], directory, stopOnError, knownHashes);
        });

        std::vector<ComputedAddItem> computed;
        computed.reserve(planned.size());
        for (size_t i = 0; i < outcomes.size(); i++) {
            auto &outcome = outcomes[i];
            if (outcome.computed)
                computed.push_back(std::move(*outcome.computed));
            else if (outcome.error)
                errors.push_back(std::move(*outcome.error));
            else if (outcome.unchanged)
                unchanged.push_back(planned[i].relPath);
        }

        return computed;
    }

    // COMMIT: Transaction(Immediate); re-verify (mtime,size) to guard against a file that
    // changed between COMPUTE and COMMIT (TOCTOU), then
    // INSERT/UPDATE. Only row writes happen inside the lock — milliseconds, not seconds.
    // Items whose (mtime,size) changed since COMPUTE are reported via `conflicts` (absolute
    // paths) instead of being written with stale metadata; the caller decides whether to
    // re-plan them (addToIndexEx) or let them self-heal on the next add/rescan (addToIndex).
    void commitAddEntries(Database *db, const std::vector<ComputedAddItem> &computed,
                          const fs::path &directory, AddCallback callback,
                          std::vector<std::pair<Entry, bool>> *accepted,
                          std::vector<fs::path> *conflicts,
                          std::vector<std::string> *unchanged) {
        if (computed.empty())
            return;

        auto insertQ = db->query(
            "INSERT INTO entries (path, hash, type, properties, mtime, size, depth, "
            "point_geom, polygon_geom) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, GeomFromText(?, 4326), GeomFromText(?, "
            "4326))");
        const auto updateQ = db->query(UPDATE_QUERY);
        const auto recheckQ = db->query("SELECT mtime,hash FROM entries WHERE path=?");

        Transaction tx(db, Transaction::Mode::Immediate);

        // Buffered locally and only merged into `accepted` after a successful commit, so a
        // canceled/rolled-back batch never reports rolled-back writes as committed.
        std::vector<std::pair<Entry, bool>> tentative;

        for (auto &ci : computed) {
            const fs::path absPath = directory / io::Path(ci.entry.path).get();

            // Re-validate filesystem state (mtime/size) between COMPUTE and COMMIT, with one
            // stat. Directories use logical size (0, or aggregate for nested DroneDBs), so
            // only the size of regular files is compared.
            bool changed = false;
            try {
                const auto state = io::Path(absPath).getState();
                const long long size = state.directory ? ci.observedSize : static_cast<long long>(state.size);
                changed = state.mtime != ci.observedMtime || size != ci.observedSize;
            } catch (const FSException &) {
                changed = true; // gone
            }

            if (changed) {
                LOGD << "Skipping '" << ci.entry.path
                    << "': changed between compute and commit (will be re-detected next add)";
                if (conflicts != nullptr)
                    conflicts->push_back(absPath);
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
                    if (unchanged != nullptr)
                        unchanged->push_back(ci.entry.path);
                    continue;
                }
                actuallyInsert = false; // treat as UPDATE instead
            } else if (stillAbsent && ci.isUpdate) {
                // Planned as UPDATE but row was deleted — re-plan via conflict retry
                LOGD << "Skipping '" << ci.entry.path
                    << "': update plan stale, row no longer exists (will be re-planned)";
                if (conflicts != nullptr)
                    conflicts->push_back(absPath);
                continue;
            }

            if (actuallyInsert) {
                insertQ->bind(1, ci.entry.path);
                insertQ->bind(2, ci.entry.hash);
                insertQ->bind(3, ci.entry.type);
                insertQ->bind(4, ci.entry.properties.dump());
                insertQ->bind(5, ci.observedMtime);
                insertQ->bind(6, static_cast<long long>(ci.entry.size));
                insertQ->bind(7, ci.entry.depth);
                insertQ->bind(8, ci.entry.point_geom.toWkt());
                insertQ->bind(9, ci.entry.polygon_geom.toWkt());
                insertQ->execute();
            } else {
                doUpdate(updateQ.get(), ci.entry, ci.observedMtime);
            }

            // Both report "updated", the opposite of "inserted"
            const bool updated = !actuallyInsert;
            tentative.push_back({ci.entry, updated});

            if (callback != nullptr)
                if (!callback(ci.entry, updated)) {
                    // tx's destructor rolls back; log it explicitly so a canceled batch
                    // is never a silent loss (C8).
                    LOGD << "Add operation canceled by callback, rolling back transaction";
                    return;
                }
        }

        tx.commit();

        if (accepted != nullptr)
            accepted->insert(accepted->end(), tentative.begin(), tentative.end());
    }

    // DDB-12: a .prj gives its shapefile or CAD drawing a coordinate system.
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
                ignored.push_back(directory / io::Path(prj).get());
        cad::IgnoredSidecarScope ignore(ignored);

        const auto updateQ = db->query(UPDATE_QUERY);
        for (const auto &prj : prjPaths) {
            for (const auto &mainPath : findSidecarMains(db, prj)) {
                Entry old;
                if (!getEntry(db, mainPath, old))
                    continue;

                try {
                    Entry fresh;
                    fresh.hash = old.hash; // content unchanged: skip re-hashing
                    parseEntry(directory / io::Path(mainPath).get(), directory, fresh, true);

                    {
                        Transaction tx(db, Transaction::Mode::Immediate);
                        doUpdate(updateQ.get(), fresh);
                        tx.commit();
                    }

                    // A GeoPackage kept open for WFS/OGC queries would block this on Windows
                    const fs::path out = db->buildDirectory() / old.hash;
                    releaseCachedFiles(out);
                    for (const auto &folder : entryHandler(old.type).artifactFolders())
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
        return io::Path(path).checkExtension({"prj"});
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

        std::vector<AddItemError> discardedErrors;
        std::vector<std::string> discardedUnchanged;
        auto planned = planAddCandidates(db, pathList, directory);
        auto computed = computeAddEntries(planned, directory, /*stopOnError=*/true, discardedErrors, discardedUnchanged,
                                          knownHashes);
        std::vector<std::pair<Entry, bool>> accepted;
        commitAddEntries(db, computed, directory, callback, &accepted, nullptr, nullptr);

        std::vector<std::string> prjPaths;
        for (const auto &a : accepted)
            if (isPrjPath(a.first.path)) prjPaths.push_back(a.first.path);
        refreshPrjMains(db, directory, prjPaths, false);
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
            auto planned = planAddCandidates(db, pathList, directory);
            auto computed = computeAddEntries(planned, directory, options.stopOnError,
                                              result.errors, result.unchanged, knownHashes);

            std::vector<fs::path> conflicts;
            commitAddEntries(db, computed, directory, callback, &result.entries, &conflicts,
                             &result.unchanged);

            pathList = conflicts; // bounded re-plan: only what actually changed under us
        }

        for (auto &p : pathList) {
            io::Path relPath = io::Path(p).relativeTo(directory);
            result.errors.push_back({relPath.generic(), "CONFLICT", "File kept changing during indexing"});
        }

        std::vector<std::string> prjPaths;
        for (size_t i = firstNewEntry; i < result.entries.size(); i++)
            if (isPrjPath(result.entries[i].first.path)) prjPaths.push_back(result.entries[i].first.path);
        refreshPrjMains(db, directory, prjPaths, false);
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

    // The entries below `folder` (an index range: '0' is the character after '/', so sibling
    // folders such as "folder2" are excluded)
    static PathPredicate descendantsPredicate(const std::string &folder, const std::string &column)
    {
        return {column + " >= ? AND " + column + " < ?", {folder + "/", folder + "0"}};
    }

    PathPredicate buildPathPredicate(const std::string &query, bool isFolder, const std::string &column)
    {
        if (query.empty())
            return {};

        if (query.find('*') != std::string::npos)
        {
            // Wildcards need LIKE, which cannot use the (case-sensitive) primary key index
            auto pattern = sanitize_query_param(query);
            if (isFolder)
                pattern += "//%";
            return {column + " LIKE ? ESCAPE '/'", {pattern}};
        }

        if (isFolder)
            return descendantsPredicate(query, column);

        return {column + " = ?", {query}};
    }

    static void bindPathPredicate(Statement &statement, const PathPredicate &predicate, int firstParam = 1)
    {
        for (size_t i = 0; i < predicate.bindings.size(); i++)
            statement.bind(firstParam + static_cast<int>(i), predicate.bindings[i]);
    }

    namespace {

    void removeBuildFolder(const fs::path &buildFolder)
    {
        if (fs::exists(buildFolder))
        {
            LOGD << "Removing " << buildFolder.string();
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

    // Removes the build folders of deleted entries, in parallel, outside any transaction.
    void deleteBuildFolders(Database *db, const std::vector<std::string> &hashes)
    {
        if (hashes.empty())
            return;

        const auto buildDir = db->buildDirectory();
        parallel::forEachIndex(hashes.size(), parallel::hardwareThreads(), [&](size_t i)
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

    int deleteFromIndex(Database *db, const std::string &query, bool isFolder, RemoveCallback callback)
    {

        LOGD << "Query: " << query;

        // An empty query never matched anything here (unlike getMatchingEntries, where it
        // means the whole index): keep it that way rather than deleting every entry.
        if (query.empty())
            return 0;

        const auto predicate = buildPathPredicate(query, isFolder);

        LOGD << "Predicate: " << predicate.sql;

        std::vector<std::string> deletedPaths, deletedHashes;
        Transaction tx(db, Transaction::Mode::Immediate);
        const int count = RowDeleter(db, predicate.sql).run(predicate, deletedPaths, deletedHashes);
        tx.commit();

        if (callback != nullptr)
            for (const auto &path : deletedPaths)
                callback(path);

        deleteBuildFolders(db, deletedHashes);

        return count;
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
            Transaction tx(db, Transaction::Mode::Immediate);

            // Every path must match before anything is removed: a path listed twice, or
            // inside a folder also listed, is then removed once instead of failing the
            // second time. Sorted, so a folder comes before its content.
            std::map<std::string, EntryType> targets;
            for (const auto &p : paths)
            {
                LOGD << "Deleting path: " << p;

                auto relPath = io::Path(p).relativeTo(directory);

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
                if (cnt > 0 && type != Directory && isPrjPath(path))
                    removedPrj.push_back(path);

                if (type == Directory && !path.empty())
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

    // Columns read by readEntryRow(). The metadata column is a correlated subquery
    // (one seek on ix_entries_meta_path_key per entry); joining entries_meta instead
    // would return each entry once per metadata row.
    static const char *ENTRY_WITH_META_SELECT = R"<<<(
        SELECT e.path, e.hash, e.type, e.properties, e.mtime, e.size, e.depth,
        json_extract(AsGeoJSON(e.point_geom), '$.coordinates'), json_extract(AsGeoJSON(e.polygon_geom), '$.coordinates'),
        (
            SELECT NULLIF(json_group_object(key, json(meta)), '{}')
            FROM (
                SELECT emi.key AS key, CASE WHEN substr(emi.key, -1, 1) = 's'
                                THEN json_group_array(json_object('id', emi.id, 'data', json(emi.data), 'mtime', emi.mtime))
                                ELSE json_object('id', emi.id, 'data', json(emi.data), 'mtime', emi.mtime)
                            END AS meta
                FROM entries_meta emi
                WHERE emi.path = e.path
                GROUP BY emi.key
            )
        ) AS meta
        FROM entries e
    )<<<";

    static void readEntryRow(Statement &q, Entry &e)
    {
        e.parseFields(q.getText(0), q.getText(1), q.getInt(2), q.getText(3),
                      q.getInt64(4), q.getInt64(5), q.getInt(6),
                      q.getText(7), q.getText(8),
                      q.getText(9));
    }

    std::vector<Entry> getMatchingEntries(Database *db, const fs::path &path,
                                          int maxRecursionDepth, bool isFolder)
    {
        // 0 is ALL_DEPTHS
        if (maxRecursionDepth < 0)
            throw FSException("Max recursion depth cannot be negative");

        const auto query = path.string();

        LOGD << "Query: " << query;

        const auto predicate = buildPathPredicate(query, isFolder, "e.path");

        LOGD << "Predicate: " << (predicate.sql.empty() ? "<all>" : predicate.sql);

        std::vector<std::string> conditions;
        if (!predicate.sql.empty())
            conditions.push_back(predicate.sql);
        if (maxRecursionDepth > 0)
            conditions.push_back("e.depth <= " + std::to_string(maxRecursionDepth - 1));

        std::string sql = ENTRY_WITH_META_SELECT;
        for (size_t i = 0; i < conditions.size(); i++)
            sql += (i == 0 ? " WHERE " : " AND ") + conditions[i];

        auto q = db->query(sql);
        bindPathPredicate(*q, predicate);

        std::vector<Entry> entries;

        while (q->fetch())
        {
            Entry e;
            readEntryRow(*q, e);
            entries.push_back(std::move(e));
        }

        q->reset();

        return entries;
    }

    namespace
    {
        // Dataset-relative folder without "." parts and trailing separators ("" = root)
        std::string normalizeFolder(const std::string &folder)
        {
            std::string normalized = fs::path(folder).lexically_normal().generic_string();
            while (!normalized.empty() && normalized.back() == '/')
                normalized.pop_back();
            if (normalized == ".")
                normalized.clear();

            const fs::path p(normalized);
            if (p.is_absolute() || p.has_root_name() || (!normalized.empty() && *p.begin() == ".."))
                throw InvalidArgsException("Invalid folder: " + folder);
            return normalized;
        }

        // LIKE pattern (escape character '/') matching every path whose file name may match
        // namePattern; matchesNamePattern() decides. '%' can span folders, so it selects more.
        // SQLite's LIKE folds ASCII letters only, so when matchesNamePattern() folds Unicode (a
        // valid UTF-8 pattern) a non-ASCII character, and an ASCII letter that a non-ASCII one
        // folds to as well (K, S), becomes '_' (one character: simple folding keeps the count).
        std::string namePatternToLike(const std::string &namePattern)
        {
            const bool unicodeFolding = utf8::isValid(namePattern);
            std::string like = "%//";
            size_t i = 0;
            while (i < namePattern.size()) {
                const char c = namePattern[i];
                char32_t cp;
                if (unicodeFolding && static_cast<unsigned char>(c) >= 0x80 &&
                    utf8::decode(namePattern, i, cp)) {
                    like += '_';
                    continue;
                }
                i++;
                if (c == '*')
                    like += '%';
                else if (c == '?' || (unicodeFolding && utf8::hasNonAsciiFoldVariant(c)))
                    like += '_';
                else
                {
                    if (c == '/' || c == '%' || c == '_')
                        like += '/';
                    like += c;
                }
            }
            return like;
        }

        std::string fileName(const std::string &path)
        {
            const auto slash = path.rfind('/');
            return slash == std::string::npos ? path : path.substr(slash + 1);
        }
    } // namespace

    std::vector<Entry> listEntries(Database *db, const EntryQuery &query)
    {
        const std::string folder = normalizeFolder(query.folder);

        std::vector<std::string> conditions;
        std::vector<std::string> bindings;
        const auto add = [&](const PathPredicate &predicate)
        {
            conditions.push_back(predicate.sql);
            bindings.insert(bindings.end(), predicate.bindings.begin(), predicate.bindings.end());
        };

        if (!folder.empty())
        {
            Entry self;
            if (getEntry(db, folder, self) && self.type != EntryType::Directory)
                add({"e.path = ?", {folder}});
            else
            {
                add(descendantsPredicate(folder, "e.path"));
                if (!query.recursive)
                    conditions.push_back("e.depth = " + std::to_string(std::count(folder.begin(), folder.end(), '/') + 1));
            }
        }
        else if (!query.recursive)
            conditions.push_back("e.depth = 0");

        if (!query.types.empty())
        {
            std::string in;
            for (const auto type : query.types)
                in += (in.empty() ? "" : ",") + std::to_string(static_cast<int>(type));
            conditions.push_back("e.type IN (" + in + ")");
        }

        if (!query.namePattern.empty())
            add({"('/' || e.path) LIKE ? ESCAPE '/'", {namePatternToLike(query.namePattern)}});

        std::string sql = query.withDetails ? ENTRY_WITH_META_SELECT
                                            : "SELECT e.path, e.hash, e.type, e.mtime, e.size, e.depth FROM entries e";
        for (size_t i = 0; i < conditions.size(); i++)
            sql += (i == 0 ? " WHERE " : " AND ") + conditions[i];
        sql += " ORDER BY e.path";

        auto q = db->query(sql);
        bindPathPredicate(*q, {"", bindings});

        std::vector<Entry> entries;
        while (q->fetch())
        {
            if (!query.namePattern.empty() && !utils::matchesNamePattern(fileName(q->getText(0)), query.namePattern))
                continue;

            Entry e;
            if (query.withDetails)
                readEntryRow(*q, e);
            else
            {
                e.path = q->getText(0);
                e.hash = q->getText(1);
                e.type = static_cast<EntryType>(q->getInt(2));
                e.mtime = static_cast<time_t>(q->getInt64(3));
                e.size = static_cast<std::uintmax_t>(q->getInt64(4));
                e.depth = q->getInt(5);
            }
            entries.push_back(std::move(e));
        }

        return entries;
    }

    void syncIndex(Database *db)
    {
        const fs::path directory = db->rootDirectory();

        auto q = db->query("SELECT path,mtime,hash FROM entries");
        auto deleteQ = db->query("DELETE FROM entries WHERE path = ?");
        const auto updateQ = db->query(UPDATE_QUERY);

        // Deliberately one IMMEDIATE transaction for the whole re-index (design choice, kept):
        // the entries table is always left in a consistent, fully re-parsed state, even for
        // consumers reading between statements. The cost is that re-index holds the sole SQLite
        // writer lock for its entire duration; concurrent addToIndexEx callers see busy and
        // retry/jitter per RetryPolicy.
        Transaction tx(db, Transaction::Mode::Immediate);

        while (q->fetch())
        {
            io::Path relPath = fs::path(q->getText(0));
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
                std::cout << "D\t" << relPath.generic() << std::endl;
                break;

            case Modified:

                parseEntry(p, directory, e, true);
                doUpdate(updateQ.get(), e, fileMtime);
                std::cout << "U\t" << e.path << std::endl;
                break;

            default:; // Do nothing
            }
        }

        tx.commit();
    }

    void rescanIndex(Database *db, const std::vector<EntryType> &types, bool stopOnError, RescanCallback callback)
    {
        rescanIndex(db, types, {}, stopOnError, callback);
    }

    void rescanIndex(Database *db, const std::vector<EntryType> &types, const std::vector<std::string> &extensions,
                     bool stopOnError, RescanCallback callback)
    {
        const fs::path directory = db->rootDirectory();

        // Build query based on requested types
        std::string sql = "SELECT path FROM entries WHERE type != ?";

        if (!types.empty())
        {
            sql += " AND type IN (";
            for (size_t i = 0; i < types.size(); i++)
            {
                if (i > 0)
                    sql += ",";
                sql += "?";
            }
            sql += ")";
        }

        auto q = db->query(sql);
        // Always exclude directories from rescan because they do not contain file metadata
        // that can be re-parsed. This check is defensive and prevents potential issues
        // even if validation fails at higher layers (C API, CLI).
        q->bind(1, EntryType::Directory);

        // Bind types if specified
        for (size_t i = 0; i < types.size(); i++)
        {
            q->bind(2 + static_cast<int>(i), static_cast<int>(types[i]));
        }

        const auto updateQ = db->query(UPDATE_QUERY);

        // Deliberately one IMMEDIATE transaction for the whole rescan (design choice, kept):
        // the entries table is always left in a consistent, fully re-parsed state, even for
        // consumers reading between statements. The cost is that re-scan holds the sole SQLite
        // writer lock for its entire duration; concurrent addToIndexEx callers see busy and
        // retry/jitter per RetryPolicy.
        Transaction tx(db, Transaction::Mode::Immediate);

        while (q->fetch())
        {
            const std::string path = q->getText(0);
            if (!extensions.empty() && !utils::hasAnyExtension(path, extensions))
                continue;

            io::Path relPath = fs::path(path);
            fs::path fullPath = directory / relPath.get();

            // Check if file still exists (outside try-catch to handle stopOnError correctly)
            if (!fs::exists(fullPath))
            {
                std::string error = "File not found: " + fullPath.string();
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
            if (fullPath.setModifiedTimePrecise(q->getDouble(1)))
            {
                LOGD << "Updated mtime for " << fullPath.string();
            }
        }
    }

    bool checkpointIndex(const std::string &directory)
    {
        const auto db = ddb::open(directory, false);
        // One row: busy (1 when blocked), WAL frames, frames checkpointed
        const auto q = db->query("PRAGMA wal_checkpoint(TRUNCATE)");
        return !q->fetch() || q->getInt(0) == 0;
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
        const auto dbasePath = ddbDirPath / DDB_DATABASE_FILE;

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
                                         (std::string("empty-dbase-") + ddb::revision() + ".sqlite");

            ensureEmptyIndexTemplate(emptyDbPath);

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
                LOGD << "Cannot find " << emptyDbPath.string()
                     << " in data path, strange! Building from scratch instead";
                fromScratch = true;
            }
        }

        if (fromScratch)
        {
            LOGD << "Creating " << dbasePath.string();

            // Create database
            Database db;
            db.open(dbasePath.string());
            db.createTables();
            db.close();
        }

        return ddbDirPath.string();
    }

    void deleteEntry(Database *db, const std::string &path)
    {

        auto f = db->query("DELETE FROM entries WHERE path = ?");
        f->bind(1, path);
        executeWithRetry([&f] { f->execute(); }, "deleteEntry");

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
        executeWithRetry([&q] { q->execute(); }, "addFolder");
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

    bool getEntryWithMeta(Database *db, const std::string &path, Entry &entry)
    {
        auto q = db->query(std::string(ENTRY_WITH_META_SELECT) + " WHERE e.path = ?");
        q->bind(1, path);

        if (!q->fetch())
            return false;

        readEntryRow(*q, entry);
        return true;
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

        const auto depth = io::Path(dest).depth();

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

        Transaction tx(db, Transaction::Mode::Immediate);

        // If we are moving a file
        if (sourceEntry.type != Directory)
        {

            if (destExists)
                deleteEntry(db, dest);

            replacePath(db, source, dest);

            // If the file extension changed, re-detect the entry type
            const auto sourceExt = fs::path(source).extension();
            const auto destExt = fs::path(dest).extension();

            if (sourceExt != destExt)
            {
                const auto newType = fingerprint(directory / dest);
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
