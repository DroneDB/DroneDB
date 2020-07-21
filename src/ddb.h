/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DDB_H
#define DDB_H

#include "database.h"
#include "statement.h"
#include "entry.h"
#include "fs.h"

namespace ddb {

void initialize(bool verbose = false);
std::string getVersion();
std::string create(const std::string &directory);
std::unique_ptr<Database> open(const std::string &directory, bool traverseUp);
fs::path rootDirectory(Database *db);
std::vector<fs::path> getIndexPathList(fs::path rootDirectory, const std::vector<std::string> &paths, bool includeDirs);
std::vector<fs::path> getPathList(const std::vector<std::string> &paths, bool includeDirs, int maxDepth);
bool checkUpdate(Entry &e, const fs::path &p, long long dbMtime, const std::string &dbHash);
void doUpdate(Statement *updateQ, const Entry &e);

void addToIndex(Database *db, const std::vector<std::string> &paths);
void removeFromIndex(Database *db, const std::vector<std::string> &paths);
void syncIndex(Database *db);


}


#endif // DDB_H
