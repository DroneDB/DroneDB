/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "index_sql.h"

#include <format>

#include "metakey.h"

namespace ddb
{

    namespace
    {
        // The metadata is a JSON object: {key: item} for a single-item key, {key: [items]} for a
        // list key (metakey.h)
        std::string buildSelect(EntryColumns columns)
        {
            std::string select = "SELECT e.path, e.hash, e.type, ";
            if (columns >= EntryColumns::Properties)
                select += "e.properties, ";
            select += "e.mtime, e.size, e.depth";
            if (columns >= EntryColumns::Geometries)
                select += ", json_extract(AsGeoJSON(e.point_geom), '$.coordinates'), "
                          "json_extract(AsGeoJSON(e.polygon_geom), '$.coordinates')";
            if (columns >= EntryColumns::Meta)
                select += ", (" + metaAggregateSql("e.path") + ") AS meta";
            return select + " FROM entries e";
        }
    } // namespace

    const std::string &selectEntries(EntryColumns columns)
    {
        static const std::string summary = buildSelect(EntryColumns::Summary);
        static const std::string properties = buildSelect(EntryColumns::Properties);
        static const std::string geometries = buildSelect(EntryColumns::Geometries);
        static const std::string meta = buildSelect(EntryColumns::Meta);

        switch (columns)
        {
        case EntryColumns::Summary:
            return summary;
        case EntryColumns::Properties:
            return properties;
        case EntryColumns::Geometries:
            return geometries;
        case EntryColumns::Meta:
            return meta;
        }
        return meta;
    }

    void readEntry(Statement &statement, EntryColumns columns, Entry &entry)
    {
        if (columns == EntryColumns::Summary)
        {
            entry.path = statement.getText(0);
            entry.hash = statement.getText(1);
            entry.type = static_cast<EntryType>(statement.getInt(2));
            entry.mtime = static_cast<time_t>(statement.getInt64(3));
            entry.size = static_cast<std::uintmax_t>(statement.getInt64(4));
            entry.depth = statement.getInt(5);
            return;
        }

        const bool geometries = columns >= EntryColumns::Geometries;
        const bool meta = columns >= EntryColumns::Meta;
        entry.parseFields(statement.getText(0), statement.getText(1), statement.getInt(2), statement.getText(3),
                          statement.getInt64(4), statement.getInt64(5), statement.getInt(6),
                          geometries ? statement.getText(7) : std::string(),
                          geometries ? statement.getText(8) : std::string(),
                          meta ? statement.getText(9) : std::string());
    }

    std::string metaAggregateSql(std::string_view pathExpression)
    {
        // {{}} is a literal "{}" for std::format
        return std::format(R"<<<(
            SELECT NULLIF(json_group_object(key, json(meta)), '{{}}')
            FROM (
                SELECT emi.key AS key, CASE WHEN substr(emi.key, -1, 1) = '{0}'
                                THEN json_group_array(json_object('id', emi.id, 'data', json(emi.data), 'mtime', emi.mtime))
                                ELSE json_object('id', emi.id, 'data', json(emi.data), 'mtime', emi.mtime)
                            END AS meta
                FROM entries_meta emi
                WHERE emi.path = {1}
                GROUP BY emi.key
            )
        )<<<",
                           metakey::kListSuffix, pathExpression);
    }

    std::string inListSql(size_t count)
    {
        std::string sql = "(";
        for (size_t i = 0; i < count; i++)
            sql += i == 0 ? "?" : ",?";
        return sql + ")";
    }

    int bindEntryTypes(Statement &statement, int firstParam, const std::vector<EntryType> &types)
    {
        return bindInList(statement, firstParam, types, [](EntryType type) { return entryTypeValue(type); });
    }

    SqlConditions &SqlConditions::add(std::string sql)
    {
        conditions_.push_back(std::move(sql));
        return *this;
    }

    SqlConditions &SqlConditions::add(const PathPredicate &predicate)
    {
        if (predicate.sql.empty())
            return *this;
        conditions_.push_back(predicate.sql);
        values_.insert(values_.end(), predicate.bindings.begin(), predicate.bindings.end());
        return *this;
    }

    SqlConditions &SqlConditions::addEquals(const std::string &column, int value)
    {
        conditions_.push_back(column + " = ?");
        values_.emplace_back(value);
        return *this;
    }

    SqlConditions &SqlConditions::addAtMost(const std::string &column, int value)
    {
        conditions_.push_back(column + " <= ?");
        values_.emplace_back(value);
        return *this;
    }

    SqlConditions &SqlConditions::addEntryTypes(const std::string &column, const std::vector<EntryType> &types)
    {
        conditions_.push_back(column + " IN " + inListSql(types.size()));
        for (const EntryType type : types)
            values_.emplace_back(entryTypeValue(type));
        return *this;
    }

    SqlConditions &SqlConditions::addInStrings(const std::string &column, const std::vector<std::string> &values)
    {
        conditions_.push_back(column + " IN " + inListSql(values.size()));
        values_.insert(values_.end(), values.begin(), values.end());
        return *this;
    }

    std::string SqlConditions::whereClause() const
    {
        std::string clause;
        for (size_t i = 0; i < conditions_.size(); i++)
            clause += (i == 0 ? " WHERE " : " AND ") + conditions_[i];
        return clause;
    }

    void SqlConditions::bind(Statement &statement, int firstParam) const
    {
        int param = firstParam;
        for (const Value &value : values_)
            std::visit([&](const auto &v) { statement.bind(param, v); }, value), param++;
    }

    const std::string &insertFolderSql()
    {
        static const std::string sql = std::format(
            "INSERT INTO entries (path, type, properties, mtime, size, depth) VALUES (?, {}, 'null', ?, 0, ?)",
            entryTypeValue(EntryType::Directory));
        return sql;
    }

    const std::string &missingFoldersSql()
    {
        static const std::string sql = std::format(
            "SELECT B.folder FROM ("
            " SELECT A.path, TRIM(A.folder, '/') AS folder FROM ("
            " SELECT path, replace(path, replace(path, rtrim(path, replace(path, '/', '')), ''), '') AS folder"
            " FROM entries WHERE {0}) AS A"
            " WHERE length(A.folder) > 0) AS B WHERE folder NOT IN (SELECT path FROM entries WHERE {1})",
            typeCondition("type", "!=", EntryType::Directory), typeCondition("type", "=", EntryType::Directory));
        return sql;
    }

    std::string typeCondition(std::string_view column, std::string_view op, EntryType type)
    {
        return std::format("{} {} {}", column, op, entryTypeValue(type));
    }

} // namespace ddb
