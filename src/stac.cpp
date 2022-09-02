/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "stac.h"
#include "logger.h"
#include "exceptions.h"
#include "mio.h"

#include "ddb.h"

namespace ddb{

std::string generateStac(const std::vector<std::string> &paths,
                         const std::string &entry,
                         const std::string &matchExpr,
                         bool recursive,
                         int maxRecursionDepth,
                         const std::string &endpoint,
                         const std::string &id){
    std::vector<std::string> ddbPaths;

    // Generate list of DDB database paths
    for (const fs::path p : paths){
        if (io::exists(p / DDB_FOLDER / "dbase.sqlite")){
            ddbPaths.push_back(p.string());
        }
    }

    if (ddbPaths.size() == 0){
        // Search
        const auto pathList = getPathList(paths, true, recursive ? maxRecursionDepth : -1, false);
        for (const fs::path &p : pathList){
            if (io::exists(p / DDB_FOLDER / "dbase.sqlite")){
                ddbPaths.push_back(p.string());
            }
        }
    }

    // Remove duplicates
    ddbPaths.erase(unique(ddbPaths.begin(), ddbPaths.end(), [](const std::string& l, const std::string& r)
        {
            return l == r;
        }), ddbPaths.end());

    if (ddbPaths.size() == 0) throw AppException("No DroneDB dataset found for generating STAC");
    LOGD << "HERE";
    json j;
    j["stac_version"] = "1.0.0";
    //j["stac_extensions"] = json::array();

    // What kind of STAC element are we generating?

    if (ddbPaths.size() == 1 && !entry.empty()){
        // STAC Item
    }else if (ddbPaths.size() == 1 && entry.empty()){
        auto db = open(ddbPaths.front(), false);

        // Stac Collection
        j["id"] = !id.empty() ? id : fs::weakly_canonical(db->rootDirectory()).filename().string();
        j["type"] = "Collection";
        j["title"] = db->getMetaManager()->getString("name", "", "", j["id"]);

        // TODO: populate with README.md content if indexed
        j["description"] = db->getMetaManager()->getString("name", "", "", j["id"]);

        j["license"] = db->getMetaManager()->getString("license", "", "", "proprietary");

        //j["links"]
        //j["extent"]

        //j["assets"] ? (optional)
    }else if (ddbPaths.size() > 1 && entry.empty()){
        // Stac Catalog
    }else{
        throw AppException("Invalid STAC generation request");
    }

    return j.dump(4);
}

}
