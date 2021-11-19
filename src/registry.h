/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef REGISTRY_H
#define REGISTRY_H

#include <entry_types.h>

#include <chrono>
#include <string>
#include <vector>

#include "fs.h"
#include "constants.h"
#include "entry.h"
#include "ddb_export.h"
#include "net.h"
#include "database.h"

namespace ddb {

class DatasetInfo;
struct Delta;

enum MergeStrategy{
    DontMerge = 0,
    KeepTheirs = 1,
    KeepOurs = 2
};

enum ConflictType{
    RemoteDeleteLocalModified = 0,
    BothModified = 1
};

struct Conflict{
    std::string path;
    ConflictType type;

    Conflict(const std::string &path, ConflictType type) :
        path(path), type(type) {}

    std::string description(){
        switch(type){
        case ConflictType::RemoteDeleteLocalModified:
            return "deleted on remote but modified locally";
        case ConflictType::BothModified:
            return "both modified";
        }
    }
};

class Registry {
    std::string url;

    // Last valid token
    std::string authToken;

    time_t tokenExpiration;

   public:
    DDB_DLL Registry(const std::string& url = DEFAULT_REGISTRY);

    DDB_DLL std::string getUrl(const std::string& path = "") const;
    DDB_DLL std::string login();

    DDB_DLL std::string getAuthToken();
    DDB_DLL std::string login(const std::string& username,
                              const std::string& password);
    DDB_DLL void ensureTokenValidity();
    DDB_DLL bool logout();
    DDB_DLL void clone(const std::string& organization,
                       const std::string& dataset, const std::string& folder,
                       std::ostream& out);

    DDB_DLL void pull(const std::string& path, const bool force, std::ostream& out);
    DDB_DLL void push(const std::string& path, const bool force, std::ostream& out);

    DDB_DLL void handleError(net::Response& res);

    DDB_DLL time_t getTokenExpiration();

    DDB_DLL Entry getDatasetInfo(const std::string& organization,
                                       const std::string& dataset);

    DDB_DLL void downloadDdb(const std::string& organization,
                             const std::string& dataset,
                             const std::string& folder);

    DDB_DLL void downloadFiles(const std::string& organization,
                               const std::string& dataset,
                               const std::vector<std::string>& files,
                               const std::string& folder);
};

DDB_DLL void applyDelta(const Delta& d, const fs::path& sourcePath, Database *destination, MergeStrategy mergeStrategy, std::ostream& out = std::cout);
DDB_DLL void ensureParentFolderExists(const fs::path& folder);

class MergeException : public AppException{
    std::vector<Conflict> conflicts;
public:
    MergeException(const std::vector<Conflict> &conflicts) : AppException("Merge exception"), conflicts(conflicts) {}
    std::vector<Conflict > getConflicts() const { return conflicts; }
};

}  // namespace ddb

#endif  // REGISTRY_H
