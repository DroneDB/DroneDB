/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef BUILD_H
#define BUILD_H

//#include "entry.h"
#include "ddb_export.h"
#include "dbops.h"

namespace ddb {

#define DEFAULT_BUILD_PATH "out_dir"

// We should add here the new buildable file types
#define ENTRY_MATCHER_QUERY "SELECT path, hash, type, meta, mtime, size, depth, AsGeoJSON(point_geom), AsGeoJSON(polygon_geom) FROM entries WHERE path LIKE '%.laz'"

DDB_DLL void build_all(Database* db, const std::string& outputPath,
                       std::ostream& output, bool force = false);
DDB_DLL void build(Database* db, const std::string& path,
                   const std::string& outputPath, std::ostream& output, bool force = false);

}

#endif // BUILD_H