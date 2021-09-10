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
#include "registryutils.h"

namespace ddb {

typedef std::function<bool(const Entry &e, bool updated)> AddCallback;
typedef std::function<void(const std::string& path)> RemoveCallback;

DDB_DLL std::unique_ptr<Database> open(const std::string &directory, bool traverseUp);
DDB_DLL std::vector<fs::path> getIndexPathList(const fs::path& rootDirectory, const std::vector<std::string> &paths, bool includeDirs);
DDB_DLL std::vector<fs::path> getPathList(const std::vector<std::string> &paths, bool includeDirs, int maxDepth);
DDB_DLL std::vector<std::string> expandPathList(const std::vector<std::string> &paths, bool recursive, int maxRecursionDepth);
DDB_DLL std::vector<Entry> getMatchingEntries(Database* db, const fs::path& path, int maxRecursionDepth = 0, bool isFolder = false);
DDB_DLL void checkDeleteBuild(Database *db, const std::string &hash);
DDB_DLL void checkDeleteMeta(Database *db, const std::string &path);
DDB_DLL int deleteFromIndex(Database* db, const std::string &query, bool isFolder = false, RemoveCallback callback = nullptr);

DDB_DLL void doUpdate(Statement *updateQ, const Entry &e);

DDB_DLL void listIndex(Database* db, const std::vector<std::string> &paths, std::ostream& out, const std::string& format, bool recursive = false, int maxRecursionDepth = 0);
DDB_DLL void addToIndex(Database *db, const std::vector<std::string> &paths, AddCallback callback = nullptr);
DDB_DLL void removeFromIndex(Database *db, const std::vector<std::string> &paths, RemoveCallback callback = nullptr);
DDB_DLL void syncIndex(Database *db);
DDB_DLL void syncLocalMTimes(Database *db, const std::vector<std::string> &files = {});
DDB_DLL void delta(Database* sourceDb, Database* targetDb, std::ostream& out, const std::string& format);
DDB_DLL void moveEntry(Database* db, const std::string& source, const std::string& dest);
DDB_DLL bool getEntry(Database* db, const std::string& path, Entry &entry);
DDB_DLL bool pathExists(Database* db, const std::string& path);

DDB_DLL std::string initIndex(const std::string &directory, bool fromScratch = false);

DDB_DLL void clone(const ddb::TagComponents& tag, const std::string& folder);

DDB_DLL void push(const std::string &registry, const bool force = false);
DDB_DLL void pull(const std::string &registry, const bool force = false);


}


#endif // DDB_H
