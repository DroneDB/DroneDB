/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef BUILD_H
#define BUILD_H

#include "ddb_export.h"
#include "dbops.h"

namespace ddb
{

    DDB_DLL bool isBuildable(Database *db, const std::string &path, std::string &subfolder);

    DDB_DLL void buildAll(Database *db, const std::string &outputPath, bool force = false, BuildCallback callback = nullptr);
    DDB_DLL void build(Database *db, const std::string &path, const std::string &outputPath, bool force = false, BuildCallback callback = nullptr);

    DDB_DLL void buildPending(Database *db, const std::string &outputPath, bool force = false, BuildCallback callback = nullptr);
    DDB_DLL bool isBuildPending(Database *db);

    DDB_DLL bool isBuildActive(Database *db, const std::string &path);

}

#endif // BUILD_H
