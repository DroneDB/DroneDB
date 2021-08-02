/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef BUILD_H
#define BUILD_H

//#include "entry.h"
#include "ddb_export.h"
#include "dbops.h"

namespace ddb {

#define DDB_BUILD_PATH "build"

DDB_DLL bool is_buildable(Database* db, const std::string& path, std::string& subfolder);

DDB_DLL void build_all(Database* db, const std::string& outputPath,
                       std::ostream& output, bool force = false);
DDB_DLL void build(Database* db, const std::string& path,
                   const std::string& outputPath, std::ostream& output, bool force = false);

}

#endif // BUILD_H
