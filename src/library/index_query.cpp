/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// Reading the index: path predicates, entry listings and searches, and their writers.
#include "dbops.h"
#include "index_internal.h"

#include "entry_filters.h"
#include "entry_path.h"
#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "stringutil.h"
#include "utf8.h"

#include <algorithm>

namespace ddb
{

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

            pathList.push_back(root.isParentOf(fs::current_path()) ? io::fromUtf8(io::Path(currentPath).generic()) : directory);
        }
        else
            pathList = io::fromUtf8(paths);

        std::vector<Entry> baseEntries;
        bool expandFolders = recursive;

        for (const fs::path &path : pathList)
        {
            io::Path relPath = io::Path(path).relativeTo(directory);

            auto pathStr = relPath.generic();

            // Let's expand only if we were asked to list a different folder
            expandFolders = expandFolders || pathStr.length() > 0;

            const auto depth = static_cast<int>(count(pathStr.begin(), pathStr.end(), '/'));
            std::vector<Entry> matches = getMatchingEntries(db, io::fromUtf8(relPath.generic()), depth + 1);
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
            if (entry.type != EntryType::Directory)
                outputEntries.emplace_back(Entry(entry));
            else
            {
                if (!isSingle || !expandFolders)
                    outputEntries.emplace_back(Entry(entry));

                if (expandFolders)
                {
                    const auto depth = recursive ? maxRecursionDepth : entry.depth + 2;
                    std::vector<Entry> entries = getMatchingEntries(db, io::fromUtf8(entry.path), depth, true);
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
        auto entries = getMatchingEntries(db, io::fromUtf8(query), 0, false);
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

    static std::string sanitize_query_param(const std::string &str)
    {
        std::string res(str);

        // TAKES INTO ACCOUNT PATHS THAT CONTAINS EVERY SORT OF STUFF
        str::replaceAll(res, "/", "//");
        str::replaceAll(res, "%", "/%");
        // str::replaceAll(res, "_", "/_");
        str::replaceAll(res, "*", "%");

        return res;
    }

    // The entries below `folder` (an index range: '0' is the character after '/', so sibling
    // folders such as "folder2" are excluded)
    PathPredicate descendantsPredicate(const std::string &folder, const std::string &column)
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

    void bindPathPredicate(Statement &statement, const PathPredicate &predicate, int firstParam)
    {
        for (size_t i = 0; i < predicate.bindings.size(); i++)
            statement.bind(firstParam + static_cast<int>(i), predicate.bindings[i]);
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

        const auto query = io::toUtf8(path);

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
            std::string normalized = io::toGenericUtf8(io::fromUtf8(folder).lexically_normal());
            while (!normalized.empty() && normalized.back() == '/')
                normalized.pop_back();
            if (normalized == ".")
                normalized.clear();

            const fs::path p = io::fromUtf8(normalized);
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
            if (!query.namePattern.empty() && !filters::matchesNamePattern(fileName(q->getText(0)), query.namePattern))
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

} // namespace ddb
