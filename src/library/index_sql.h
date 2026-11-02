/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef INDEX_SQL_H
#define INDEX_SQL_H

// The SQL vocabulary of the index tables, in one place (private to libddb): the entry columns
// and how a row becomes an Entry, the metadata aggregate, IN lists, WHERE clauses, the folder
// rows and the entry-type numbers that SQL needs. The index_*.cpp files, build_dispatch.cpp,
// database.cpp and stac.cpp compose their queries from here instead of repeating the text.

#include <cstddef>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "dbops.h"
#include "entry.h"
#include "entry_types.h"
#include "statement.h"

namespace ddb
{

    // ----------------------------------------------------------------------------- entry rows

    // Which columns of an `entries` row an Entry is read from; each level adds to the previous
    enum class EntryColumns
    {
        Summary,    // path, hash, type, mtime, size, depth
        Properties, // + properties
        Geometries, // + the point and polygon footprints (GeoJSON coordinates)
        Meta        // + the metadata of the entry, as the JSON object parseMeta reads
    };

    /// "SELECT <columns> FROM entries e": the caller appends WHERE/ORDER BY and refers to the
    /// table as `e`. The Meta level reads the metadata with a correlated subquery (one seek on
    /// ix_entries_meta_path_key per entry); joining entries_meta instead would return each entry
    /// once per metadata row.
    const std::string &selectEntries(EntryColumns columns);

    /// Fills `entry` from the current row of a statement built on selectEntries(columns). The
    /// fields of the levels not selected are left empty, as parseFields() does for "".
    void readEntry(Statement &statement, EntryColumns columns, Entry &entry);

    // -------------------------------------------------------------------------------- metadata

    /// SQL of a `SELECT` returning the metadata of the entry named by `pathExpression` (an SQL
    /// expression: `e.path`, `''`) as one JSON object {key: item or [items]}, or NULL when it has
    /// none. A key that ends with the list suffix (metakey.h) holds an array. The expression may
    /// refer to the enclosing query, so the SELECT can be used as a scalar subquery.
    std::string metaAggregateSql(std::string_view pathExpression);

    // ---------------------------------------------------------------------------- IN lists

    /// "(?,?,?)" for `count` values (count > 0)
    std::string inListSql(size_t count);

    /// Binds `values` to the parameters of an inListSql() from `firstParam` on; `bindable` maps
    /// a value to what Statement::bind takes (an int, long long, double or std::string).
    /// Returns the next free parameter number.
    template <class Range, class Bindable>
    int bindInList(Statement &statement, int firstParam, const Range &values, Bindable bindable)
    {
        int param = firstParam;
        for (const auto &value : values)
            statement.bind(param++, bindable(value));
        return param;
    }

    /// bindInList() for entry types: binds their numbers
    int bindEntryTypes(Statement &statement, int firstParam, const std::vector<EntryType> &types);

    // --------------------------------------------------------------------------- WHERE clause

    /// AND-ed conditions with the values of their parameters, bound in the order they were added
    class SqlConditions
    {
        using Value = std::variant<int, std::string>;
        std::vector<std::string> conditions_;
        std::vector<Value> values_;

    public:
        /// A condition without parameters
        SqlConditions &add(std::string sql);
        /// A path predicate (an empty one adds nothing)
        SqlConditions &add(const PathPredicate &predicate);
        /// `column = ?`
        SqlConditions &addEquals(const std::string &column, int value);
        /// `column <= ?`
        SqlConditions &addAtMost(const std::string &column, int value);
        /// `column IN (?,?,...)` (a non-empty list)
        SqlConditions &addEntryTypes(const std::string &column, const std::vector<EntryType> &types);
        SqlConditions &addInStrings(const std::string &column, const std::vector<std::string> &values);

        /// "" without conditions, otherwise " WHERE a AND b"
        std::string whereClause() const;
        /// Binds the values to the parameters of whereClause() from `firstParam` on
        void bind(Statement &statement, int firstParam = 1) const;
    };

    // ------------------------------------------------------------------------- folders, types

    /// Entry rows that are folders have no hash, no size and 'null' properties
    const std::string &insertFolderSql();
    /// The folders that hold an indexed file but have no row of their own (the first column)
    const std::string &missingFoldersSql();

    /// `<column> <op> <number of the type>`, e.g. typeCondition("type", "!=", EntryType::Directory):
    /// the numbers are stored in the index, SQL text takes them from the enum
    std::string typeCondition(std::string_view column, std::string_view op, EntryType type);

} // namespace ddb

#endif // INDEX_SQL_H
