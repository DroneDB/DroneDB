/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DBOPS_H
#define DBOPS_H

#include <functional>
#include <optional>
#include <utility>

#include "database.h"
#include "entry_filters.h"
#include "statement.h"
#include "entry.h"
#include "entry_types.h"
#include "fs.h"
#include "output_format.h"

namespace ddb
{

    typedef std::function<bool(const Entry &e, bool updated)> AddCallback;
    typedef std::function<void(const std::string &path)> RemoveCallback;
    /// Told of every change syncIndex applies: `deleted` (the file is gone, its row was removed) or
    /// updated (its row was re-parsed), with the path
    typedef std::function<void(bool deleted, const std::string &path)> SyncCallback;
    typedef std::function<bool(const Entry &e, bool success, const std::string &error)> RescanCallback;
    /// Told, for each entry a rescan re-read, the build key it has now (build_layout.h buildKey) and whether
    /// that key is not the one of the build it had before (D-020): its build must be made again
    typedef std::function<void(const Entry &e, const std::string &buildKey, bool buildKeyChanged)> RescanKeyCallback;

    // Options for addToIndexEx / DDBAddWithOptions.
    struct AddOptions {
        bool stopOnError = true;      // default == today's addToIndex() semantics
        int maxConflictRetries = 2;   // bounded re-plan passes on TOCTOU conflict
        // A listed folder is walked and its files indexed (true), or indexed as the folder
        // alone (false). Files listed with known hashes are never walked (see addToIndexEx).
        bool recursive = true;
    };

    // One item-scoped failure (a corrupt/unreadable file), as opposed to a database-scoped
    // failure (disk full, schema corruption) which always aborts the whole batch.
    struct AddItemError {
        std::string path;
        std::string code;    // "FS" | "GDAL" | "PDAL" | "JSON" | "INDEX" | "CONFLICT"
        std::string message;
    };

    // Every path passed to addToIndexEx appears in exactly one of entries/unchanged/errors
    // (completeness contract).
    struct AddResult {
        std::vector<std::pair<Entry, bool>> entries; // Entry + isUpdate (added=false, updated=true)
        std::vector<std::string> unchanged;
        std::vector<AddItemError> errors;
    };

    /// JSON of the add outputs (DDBAddWithOptions, DDBAddWithHints): an item error is
    /// {"path", "code", "message"}; a result is {"entries": [<entry> + "status": "added" |
    /// "updated"], "unchanged": [{"path"}], "errors": [<item error>]}
    void to_json(json &j, const AddItemError &error);
    void to_json(json &j, const AddResult &result);

    /// The AddItemError code of an item-scoped failure ("FS", "GDAL", "PDAL", "JSON" or
    /// "INDEX"), or nullptr when ex is not one of those exception types.
    const char *addErrorCode(const std::exception &ex) noexcept;

    /// True when one of the components of p is the ".ddb" folder (isReservedSegment, entry_path.h;
    /// a substring such as "a.ddbfile.txt" or "x.ddb-y" does not count).
    bool containsDdbSegment(const fs::path &p);

    std::unique_ptr<Database> open(const std::string &directory, bool traverseUp);

    /// Moves the committed changes of the index of directory from its WAL into the database
    /// file and empties the WAL (TRUNCATE checkpoint), so that the database file alone holds
    /// the data, e.g. before copying it. False when a reader or a writer of another
    /// connection kept it from completing (the WAL then still holds part of the data).
    bool checkpointIndex(const std::string &directory);
    /**
     * The absolute paths to index for `paths`: each file once, the folders (with includeDirs)
     * including the parents of every path up to rootDirectory, and with `recursive` the
     * content of every listed folder.
     */
    std::vector<fs::path> getIndexPathList(const fs::path &rootDirectory, const std::vector<std::string> &paths, bool includeDirs, bool recursive = true);

    /// A listed path that cannot be indexed, and why
    struct RejectedPath
    {
        fs::path path;
        std::string reason;
    };

    /// What getIndexPathList expands `paths` to, plus the listed paths it refuses: a name that
    /// hasIndexableName refuses (it contains a path separator) or a ".ddb" segment. Files found
    /// by walking a listed folder are not reported when their name is refused: they are skipped.
    struct IndexPathList
    {
        std::vector<fs::path> paths;
        std::vector<RejectedPath> rejected;
    };
    IndexPathList expandIndexPaths(const fs::path &rootDirectory, const std::vector<std::string> &paths, bool includeDirs, bool recursive = true);
    std::vector<fs::path> getPathList(const std::vector<std::string> &paths, bool includeDirs, int maxDepth, bool includeFiles = true);

    /**
     * Legacy path expander preserved for ABI compatibility (Node.js bindings,
     * external C consumers). New CLI code should use `expandGlobPatterns`.
     */
    std::vector<std::string> expandPathList(const std::vector<std::string> &paths, bool recursive, int maxRecursionDepth);

    /**
     * Expands a list of literal paths and shell-style glob patterns
     * (`*`, `?`, `[...]`, `**`) into absolute file paths. Bare directories
     * are recursively expanded. Entries inside `.ddb` are always skipped.
     * Emits a warning to stderr for patterns that match nothing; throws
     * if no patterns matched anything at all.
     */
    std::vector<std::string> expandGlobPatterns(const std::vector<std::string> &patterns);

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
    PathPredicate buildPathPredicate(const std::string &query, bool isFolder, const std::string &column = "path");

    std::vector<Entry> getMatchingEntries(Database *db, const fs::path &path, int maxRecursionDepth = 0, bool isFolder = false);

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
    std::vector<Entry> listEntries(Database *db, const EntryQuery &query);

    // Adds members to the JSON of an entry before writeEntries writes it
    using EntryJsonDecorator = std::function<void(const Entry &, json &)>;

    // Writes entries as text (one path per line) or JSON (an array of to_json(Entry)), one entry at
    // a time; decorate (json only) adds members to each entry's JSON. Any other format throws
    // an InvalidArgsException.
    void writeEntries(std::ostream &out, const std::vector<Entry> &entries, OutputFormat format,
                      const EntryJsonDecorator &decorate = nullptr);
    /// Removes the build folder "<build root>/<key>" (the build key of an entry, see build_layout.h)
    void checkDeleteBuild(Database *db, const std::string &key);

    /// Records the warnings of a build of the entry `e` in the index (applyBuildWarnings), in the
    /// row at its path while that row still has its hash: a file replaced while it was built keeps
    /// the properties of its own parse. Nothing is written when they do not change the properties.
    void recordBuildWarnings(Database *db, const Entry &e, const std::vector<BuildWarning> &warnings);
    void checkDeleteMeta(Database *db, const std::string &path);

    void doUpdate(Statement *updateQ, const Entry &e);
    // doUpdate() storing `mtime` (with sub-second precision) instead of e.mtime
    void doUpdate(Statement *updateQ, const Entry &e, double mtime);

    // listIndex and searchIndex write text or JSON (writeEntries). Hosts that hold the format as
    // text parse it first (parseOutputFormat): an unknown name throws an InvalidArgsException.
    void listIndex(Database *db, const std::vector<std::string> &paths, std::ostream &out, OutputFormat format, bool recursive = false, int maxRecursionDepth = 0);
    void searchIndex(Database *db, const std::string &query, std::ostream &out, OutputFormat format);
    void addToIndex(Database *db, const std::vector<std::string> &paths, AddCallback callback = nullptr);

    // A file (absolute path) with the SHA-256 the caller already knows; empty for folders.
    // `size` (>= 0) and `mtime` (> 0, seconds since the epoch) are what the caller saw when it
    // computed the hash: when set, the hash is trusted only if the file on disk still has them,
    // otherwise the file is hashed as usual. Leave them unset only for hashes verified against
    // the file content (applyDelta).
    struct KnownFile {
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
    void addToIndexWithHashes(Database *db, const std::vector<KnownFile> &files, AddCallback callback = nullptr);

    /**
     * Batch add with per-item error isolation.
     * With options.stopOnError == false, an item-scoped failure (FSException, GDALException,
     * PDALException, JSONException, IndexException) is captured in result.errors instead of
     * aborting the batch; the other items still commit. Database-scoped failures (DBException,
     * DBBusyException, std::bad_alloc) always abort the whole call regardless of stopOnError.
     */
    void addToIndexEx(Database *db, const std::vector<std::string> &paths,
                      const AddOptions &options, AddResult &result, AddCallback callback = nullptr);

    /**
     * addToIndexEx() for files whose SHA-256 the caller computed while writing them (the
     * Registry hashes uploads as they stream to disk). A hash whose size/mtime still match the
     * file is used instead of reading the file again; any other file is hashed as usual.
     */
    void addToIndexEx(Database *db, const std::vector<KnownFile> &files,
                      const AddOptions &options, AddResult &result, AddCallback callback = nullptr);

    void removeFromIndex(Database *db, const std::vector<std::string> &paths, RemoveCallback callback = nullptr);
    /// Brings the index in line with the files on disk: removes the rows of deleted files and
    /// re-parses the modified ones, in one transaction; `callback` is told of each change
    void syncIndex(Database *db, SyncCallback callback = nullptr);

    /**
     * The entries a rescan re-reads. An entry must match every filter that is not empty.
     *   - types:      the entry types (directories are never re-read).
     *   - extensions: the file extensions, lowercase and without the dot (see filters::parseExtensionList).
     *   - paths:      entry paths (normalized, see requireEntryPath); a folder names the entries below it.
     *                 Empty: the whole index.
     */
    struct RescanScope
    {
        std::vector<EntryType> types;
        std::vector<std::string> extensions;
        std::vector<std::string> paths;
    };

    /**
     * Rescans entries in the index, updating their metadata and status.
     *
     * @param db           Pointer to the Database to operate on. Must be valid and open.
     * @param scope        The entries to re-read (see RescanScope).
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
     * @param keyCallback  Optional, called with the entry just before callback when the entry was re-read:
     *                     its build key, and whether that key changed. The key before the rescan is the
     *                     key the index row had (its hash, and its georeference for a type whose build
     *                     depends on it), or the hash, the name of the folders before the build key, when
     *                     there is no folder of that key but one of the hash (so the first rescan after the
     *                     upgrade reports the built models and splats that have a georeference, and nothing
     *                     else, as changed; an entry with no build at all is not reported again).
     *
     * Whole index (no paths): one transaction holds the writer lock for the whole run, so the index is
     * either fully re-read or left as it was (a cancel or a stop rolls back). Once it is committed the
     * pending markers of the layout before the build key become the markers of their entries.
     *
     * Paths: the dataset stays live, so each entry is parsed outside the writer lock and written in a
     * transaction of its own; the entries written before a cancel or a stop stay written. A path that is not
     * in the index fails with "Not in index" (EntryNotFoundException with stopOnError) before any entry is
     * re-read. An entry whose file changed, or whose row was removed, while it was parsed is not written: it
     * fails, and the add or the remove that changed it keeps the index right.
     *
     * Exceptions:
     *   - Throws AppException if stopOnError is true and an error occurs.
     */
    void rescanIndex(Database *db, const RescanScope &scope, bool stopOnError = true,
                     RescanCallback callback = nullptr, RescanKeyCallback keyCallback = nullptr);

    /** Rescans the entries of `types` and `extensions` in the whole index (see the RescanScope overload) */
    inline void rescanIndex(Database *db, const std::vector<EntryType> &types,
                            const std::vector<std::string> &extensions, bool stopOnError = true,
                            RescanCallback callback = nullptr, RescanKeyCallback keyCallback = nullptr)
    {
        rescanIndex(db, RescanScope{types, extensions, {}}, stopOnError, std::move(callback),
                    std::move(keyCallback));
    }
    void syncLocalMTimes(Database *db, const std::vector<std::string> &files = {});
    void moveEntry(Database *db, const std::string &source, const std::string &dest);
    /** Exact lookup of one entry (without its metadata); nothing when `path` is not indexed. */
    std::optional<Entry> getEntry(Database *db, const std::string &path);

    /** Exact lookup of one entry, including its metadata (entry.meta); nothing when not found. */
    std::optional<Entry> getEntryWithMeta(Database *db, const std::string &path);

    std::string initIndex(const std::string &directory, bool fromScratch = false);

}

#endif // DDB_H
