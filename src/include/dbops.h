/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DBOPS_H
#define DBOPS_H

#include <functional>
#include <optional>

#include "database.h"
#include "entry_filters.h"
#include "statement.h"
#include "entry.h"
#include "entry_types.h"
#include "fs.h"
#include "ddb_export.h"

namespace ddb
{

    typedef std::function<bool(const Entry &e, bool updated)> AddCallback;
    typedef std::function<void(const std::string &path)> RemoveCallback;
    typedef std::function<bool(const Entry &e, bool success, const std::string &error)> RescanCallback;

    // Options for addToIndexEx / DDBAddWithOptions.
    struct DDB_DLL AddOptions {
        bool stopOnError = true;      // default == today's addToIndex() semantics
        int maxConflictRetries = 2;   // bounded re-plan passes on TOCTOU conflict
    };

    // One item-scoped failure (a corrupt/unreadable file), as opposed to a database-scoped
    // failure (disk full, schema corruption) which always aborts the whole batch.
    struct DDB_DLL AddItemError {
        std::string path;
        std::string code;    // "FS" | "GDAL" | "PDAL" | "JSON" | "INDEX" | "CONFLICT"
        std::string message;
    };

    // Every path passed to addToIndexEx appears in exactly one of entries/unchanged/errors
    // (completeness contract).
    struct DDB_DLL AddResult {
        std::vector<std::pair<Entry, bool>> entries; // Entry + isUpdate (added=false, updated=true)
        std::vector<std::string> unchanged;
        std::vector<AddItemError> errors;
    };

    /// The AddItemError code of an item-scoped failure ("FS", "GDAL", "PDAL", "JSON" or
    /// "INDEX"), or nullptr when ex is not one of those exception types.
    DDB_DLL const char *addErrorCode(const std::exception &ex) noexcept;

    /// True when one of the components of p is the ".ddb" folder (isReservedSegment, entry_path.h;
    /// a substring such as "a.ddbfile.txt" or "x.ddb-y" does not count).
    DDB_DLL bool containsDdbSegment(const fs::path &p);

    DDB_DLL std::unique_ptr<Database> open(const std::string &directory, bool traverseUp);

    /// Moves the committed changes of the index of directory from its WAL into the database
    /// file and empties the WAL (TRUNCATE checkpoint), so that the database file alone holds
    /// the data, e.g. before copying it. False when a reader or a writer of another
    /// connection kept it from completing (the WAL then still holds part of the data).
    DDB_DLL bool checkpointIndex(const std::string &directory);
    /**
     * The absolute paths to index for `paths`: each file once, the folders (with includeDirs)
     * including the parents of every path up to rootDirectory, and with `recursive` the
     * content of every listed folder.
     */
    DDB_DLL std::vector<fs::path> getIndexPathList(const fs::path &rootDirectory, const std::vector<std::string> &paths, bool includeDirs, bool recursive = true);
    DDB_DLL std::vector<fs::path> getPathList(const std::vector<std::string> &paths, bool includeDirs, int maxDepth, bool includeFiles = true);

    /**
     * Legacy path expander preserved for ABI compatibility (Node.js bindings,
     * external C consumers). New CLI code should use `expandGlobPatterns`.
     */
    DDB_DLL std::vector<std::string> expandPathList(const std::vector<std::string> &paths, bool recursive, int maxRecursionDepth);

    /**
     * Expands a list of literal paths and shell-style glob patterns
     * (`*`, `?`, `[...]`, `**`) into absolute file paths. Bare directories
     * are recursively expanded. Entries inside `.ddb` are always skipped.
     * Emits a warning to stderr for patterns that match nothing; throws
     * if no patterns matched anything at all.
     */
    DDB_DLL std::vector<std::string> expandGlobPatterns(const std::vector<std::string> &patterns);

    /**
     * SQL predicate on a path column, with its bindings. Built by buildPathPredicate().
     * An empty sql matches every row.
     */
    struct PathPredicate
    {
        std::string sql;
        std::vector<std::string> bindings;
    };

    /**
     * Picks the cheapest predicate that selects `query` on `column`:
     * - no `*`, isFolder == false: `column = ?` (primary key seek, case-sensitive)
     * - no `*`, isFolder == true: the descendants of the folder, `column >= 'query/' AND column < 'query0'`
     *   ('0' is the character after '/', so sibling folders such as "query2" are excluded)
     * - `*` present: `column LIKE ? ESCAPE '/'` with `*` as the wildcard (table scan)
     * An empty query matches every entry.
     */
    DDB_DLL PathPredicate buildPathPredicate(const std::string &query, bool isFolder, const std::string &column = "path");

    DDB_DLL std::vector<Entry> getMatchingEntries(Database *db, const fs::path &path, int maxRecursionDepth = 0, bool isFolder = false);

    /**
     * Which entries listEntries() returns. Every filter is applied by the index query, so
     * the entries that do not match are never read.
     */
    struct EntryQuery
    {
        // Dataset-relative folder ("" = the dataset root). A file selects itself.
        std::string folder;
        // The whole subtree of the folder, or its children only
        bool recursive = true;
        // File name pattern: '*' matches any characters, '?' one character; letters match
        // regardless of case (filters::matchesNamePattern). Empty matches every name.
        std::string namePattern;
        // Entry types to return; empty returns every type
        std::vector<EntryType> types;
        // With properties, geometries and meta; false reads path, hash, type, mtime, size
        // and depth only
        bool withDetails = true;
    };

    // The entries selected by `query`, sorted by path
    DDB_DLL std::vector<Entry> listEntries(Database *db, const EntryQuery &query);

    // Adds members to the JSON of an entry before writeEntries writes it
    using EntryJsonDecorator = std::function<void(const Entry &, json &)>;

    // Writes entries in "text" (one path per line) or "json" (array of Entry::toJSON) format,
    // one entry at a time; decorate (json only) adds members to each entry's JSON
    DDB_DLL void writeEntries(std::ostream &out, const std::vector<Entry> &entries, const std::string &format,
                              const EntryJsonDecorator &decorate = nullptr);
    DDB_DLL void checkDeleteBuild(Database *db, const std::string &hash);
    DDB_DLL void checkDeleteMeta(Database *db, const std::string &path);
    DDB_DLL int deleteFromIndex(Database *db, const std::string &query, bool isFolder = false, RemoveCallback callback = nullptr);

    DDB_DLL void doUpdate(Statement *updateQ, const Entry &e);
    // doUpdate() storing `mtime` (with sub-second precision) instead of e.mtime
    DDB_DLL void doUpdate(Statement *updateQ, const Entry &e, double mtime);

    DDB_DLL void listIndex(Database *db, const std::vector<std::string> &paths, std::ostream &out, const std::string &format, bool recursive = false, int maxRecursionDepth = 0);
    DDB_DLL void searchIndex(Database *db, const std::string &query, std::ostream &out, const std::string &format);
    DDB_DLL void addToIndex(Database *db, const std::vector<std::string> &paths, AddCallback callback = nullptr);

    // A file (absolute path) with the SHA-256 the caller already knows; empty for folders.
    // `size` (>= 0) and `mtime` (> 0, seconds since the epoch) are what the caller saw when it
    // computed the hash: when set, the hash is trusted only if the file on disk still has them,
    // otherwise the file is hashed as usual. Leave them unset only for hashes verified against
    // the file content (applyDelta).
    struct DDB_DLL KnownFile {
        std::string path;
        std::string hash;
        long long size = -1;
        double mtime = 0;
    };

    /**
     * addToIndex() for files whose SHA-256 the caller already trusts (applyDelta: the delta
     * carries it, and the caller verified it when needed). These files are not hashed again,
     * and an indexed file is updated when its hash differs. Metadata is still parsed.
     * One write transaction for the whole list.
     */
    DDB_DLL void addToIndexWithHashes(Database *db, const std::vector<KnownFile> &files, AddCallback callback = nullptr);

    /**
     * Batch add with per-item error isolation.
     * With options.stopOnError == false, an item-scoped failure (FSException, GDALException,
     * PDALException, JSONException, IndexException) is captured in result.errors instead of
     * aborting the batch; the other items still commit. Database-scoped failures (DBException,
     * DBBusyException, std::bad_alloc) always abort the whole call regardless of stopOnError.
     */
    DDB_DLL void addToIndexEx(Database *db, const std::vector<std::string> &paths,
                              const AddOptions &options, AddResult &result, AddCallback callback = nullptr);

    /**
     * addToIndexEx() for files whose SHA-256 the caller computed while writing them (the
     * Registry hashes uploads as they stream to disk). A hash whose size/mtime still match the
     * file is used instead of reading the file again; any other file is hashed as usual.
     */
    DDB_DLL void addToIndexEx(Database *db, const std::vector<KnownFile> &files,
                              const AddOptions &options, AddResult &result, AddCallback callback = nullptr);

    DDB_DLL void removeFromIndex(Database *db, const std::vector<std::string> &paths, RemoveCallback callback = nullptr);
    DDB_DLL void syncIndex(Database *db);

    /**
     * Rescans entries in the index, updating their metadata and status.
     *
     * @param db           Pointer to the Database to operate on. Must be valid and open.
     * @param types        Optional list of EntryType values to restrict which entries are rescanned.
     *                     If empty, all entry types are rescanned.
     * @param extensions   Optional list of file extensions (lowercase, without the dot, see
     *                     filters::parseExtensionList) to restrict which entries are rescanned.
     *                     If empty, all extensions are rescanned. An entry must match both filters.
     * @param stopOnError  If true, the function throws an exception (AppException) on the first error encountered.
     *                     If false, errors are reported via the callback and rescanning continues.
     * @param callback     Optional callback function called for each entry processed.
     *                     Signature: bool(const Entry &e, bool success, const std::string &error)
     *                     - e: The entry being rescanned.
     *                     - success: True if the rescan succeeded, false otherwise.
     *                     - error: Error message if success is false, empty otherwise.
     *                     The callback's return value:
     *                     - true: Continue rescanning.
     *                     - false: Cancel the operation immediately.
     *                     If callback is nullptr, no per-entry notification is performed.
     *
     * Exceptions:
     *   - Throws AppException if stopOnError is true and an error occurs.
     *
     * Thread Safety:
     *   - Uses database transactions internally; not safe for concurrent modification of the same database.
     */
    DDB_DLL void rescanIndex(Database *db, const std::vector<EntryType> &types,
                             const std::vector<std::string> &extensions, bool stopOnError = true,
                             RescanCallback callback = nullptr);
    DDB_DLL void rescanIndex(Database *db, const std::vector<EntryType> &types = {}, bool stopOnError = true, RescanCallback callback = nullptr);
    DDB_DLL void syncLocalMTimes(Database *db, const std::vector<std::string> &files = {});
    DDB_DLL void moveEntry(Database *db, const std::string &source, const std::string &dest);
    /** Exact lookup of one entry (without its metadata); nothing when `path` is not indexed. */
    DDB_DLL std::optional<Entry> getEntry(Database *db, const std::string &path);

    /** Exact lookup of one entry, including its metadata (entry.meta); nothing when not found. */
    DDB_DLL std::optional<Entry> getEntryWithMeta(Database *db, const std::string &path);
    DDB_DLL bool pathExists(Database *db, const std::string &path);

    DDB_DLL std::string initIndex(const std::string &directory, bool fromScratch = false);

}

#endif // DDB_H
