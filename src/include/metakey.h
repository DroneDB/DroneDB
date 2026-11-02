/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef METAKEY_H
#define METAKEY_H

#include <string_view>

// The metadata key rule, in one place: a key that ends with 's' names a list (several items,
// e.g. "annotations"), any other key names a single item (e.g. "name"). MetaManager enforces it
// when metadata is written; the SQL that aggregates metadata (index_sql.h) and the CLI use it
// to decide between an array and an object.
namespace ddb::metakey
{

    inline constexpr char kListSuffix = 's';

    constexpr bool isList(std::string_view key) noexcept
    {
        return !key.empty() && key.back() == kListSuffix;
    }

} // namespace ddb::metakey

#endif // METAKEY_H
