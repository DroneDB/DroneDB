/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// Reading the index: path predicates, entry listings and searches, and their writers.
#include "dbops.h"
#include "index_internal.h"

#include "entry_filters.h"
#include "entry_path.h"
#include "exceptions.h"
#include "index_sql.h"
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

        SqlConditions conditions;
        conditions.add(predicate);
        if (maxRecursionDepth > 0)
            conditions.addAtMost("e.depth", maxRecursionDepth - 1);

        auto q = db->query(selectEntries(EntryColumns::Meta) + conditions.whereClause());
        conditions.bind(*q);

        std::vector<Entry> entries;

        while (q->fetch())
        {
            Entry e;
            readEntry(*q, EntryColumns::Meta, e);
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

        SqlConditions conditions;

        if (!folder.empty())
        {
            Entry self;
            if (getEntry(db, folder, self) && self.type != EntryType::Directory)
                conditions.add(PathPredicate{"e.path = ?", {folder}});
            else
            {
                conditions.add(descendantsPredicate(folder, "e.path"));
                if (!query.recursive)
                    conditions.addEquals("e.depth", static_cast<int>(std::count(folder.begin(), folder.end(), '/')) + 1);
            }
        }
        else if (!query.recursive)
            conditions.addEquals("e.depth", 0);

        if (!query.types.empty())
            conditions.addEntryTypes("e.type", query.types);

        if (!query.namePattern.empty())
            conditions.add(PathPredicate{"('/' || e.path) LIKE ? ESCAPE '/'", {namePatternToLike(query.namePattern)}});

        const EntryColumns columns = query.withDetails ? EntryColumns::Meta : EntryColumns::Summary;
        auto q = db->query(selectEntries(columns) + conditions.whereClause() + " ORDER BY e.path");
        conditions.bind(*q);

        std::vector<Entry> entries;
        while (q->fetch())
        {
            if (!query.namePattern.empty() && !filters::matchesNamePattern(fileName(q->getText(0)), query.namePattern))
                continue;

            Entry e;
            readEntry(*q, columns, e);
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

    // The entry stored at `path`, read up to `columns`
    static bool readEntryAt(Database *db, const std::string &path, EntryColumns columns, Entry &entry)
    {
        auto q = db->query(selectEntries(columns) + " WHERE e.path = ?");
        q->bind(1, path);

        if (!q->fetch())
            return false;

        readEntry(*q, columns, entry);
        return true;
    }

    bool getEntry(Database *db, const std::string &path, Entry &entry)
    {
        return readEntryAt(db, path, EntryColumns::Geometries, entry);
    }

    bool getEntryWithMeta(Database *db, const std::string &path, Entry &entry)
    {
        return readEntryAt(db, path, EntryColumns::Meta, entry);
    }

} // namespace ddb
