/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef INDEX_INTERNAL_H
#define INDEX_INTERNAL_H

// What the index_*.cpp files of libddb share and dbops.h does not export: the SQL of the
// entry writes and the path-predicate helpers.

#include <string>
#include <vector>

#include "dbops.h"

namespace ddb
{

    // Rewrites every column of the entry stored at the path (the last parameter). The parameters
    // follow doUpdate().
    inline constexpr const char *kUpdateEntrySql =
        "UPDATE entries SET hash=?, type=?, properties=?, mtime=?, size=?, depth=?, "
        "point_geom=GeomFromText(?, 4326), polygon_geom=GeomFromText(?, 4326) "
        "WHERE path=?";

    // The entries below `folder` (an index range: '0' is the character after '/', so sibling
    // folders such as "folder2" are excluded)
    PathPredicate descendantsPredicate(const std::string &folder, const std::string &column);

    // Binds predicate.bindings to the parameters of the statement from firstParam on
    void bindPathPredicate(Statement &statement, const PathPredicate &predicate, int firstParam = 1);

    // True for a .prj file: it gives its shapefile or CAD drawing a coordinate system
    bool isPrjPath(const std::string &path);

    // DDB-12: after a .prj was added, changed or removed, re-parses its main entries and drops
    // their stale vector artifacts. Best effort per file, runs after the add/remove transaction.
    void refreshPrjMains(Database *db, const fs::path &directory, const std::vector<std::string> &prjPaths,
                         bool removed);

} // namespace ddb

#endif // INDEX_INTERNAL_H
