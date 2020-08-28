/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DBOPS_H
#define DBOPS_H

#include "database.h"
#include "statement.h"
#include "entry.h"
#include "fs.h"
#include "ddb_export.h"

namespace ddb {

DDB_DLL std::unique_ptr<Database> open(const std::string &directory, bool traverseUp);
DDB_DLL fs::path rootDirectory(Database *db);
DDB_DLL std::vector<fs::path> getIndexPathList(fs::path rootDirectory, const std::vector<std::string> &paths, bool includeDirs);
DDB_DLL std::vector<fs::path> getPathList(const std::vector<std::string> &paths, bool includeDirs, int maxDepth);
DDB_DLL std::vector<std::string> expandPathList(const std::vector<std::string> &paths, bool recursive, int maxRecursionDepth);

DDB_DLL bool checkUpdate(Entry &e, const fs::path &p, long long dbMtime, const std::string &dbHash);
DDB_DLL void doUpdate(Statement *updateQ, const Entry &e);

DDB_DLL void addToIndex(Database *db, const std::vector<std::string> &paths);
DDB_DLL void removeFromIndex(Database *db, const std::vector<std::string> &paths);
DDB_DLL void syncIndex(Database *db);


}


#endif // DDB_H
