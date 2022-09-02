/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "stac.h"
#include "logger.h"
#include "exceptions.h"
#include "mio.h"
#include "dbops.h"
#include "ddb.h"

namespace ddb{

std::string generateStac(const std::vector<std::string> &paths,
                         const std::string &matchExpr,
                         bool recursive,
                         int maxRecursionDepth){
    std::vector<std::string> ddbPaths;

    // Generate list of DDB database paths
    for (const fs::path &p : paths){
        if (fs::exists(p / DDB_FOLDER / "dbase.sqlite")){
            ddbPaths.push_back(p.string());
        }
    }

    if (ddbPaths.size() == 0){
        // Search
        const auto pathList = getPathList(paths, true, maxRecursionDepth false);
        for (const fs::path &p : pathList){
            if (fs::exists(p / DDB_FOLDER / "dbase.sqlite")){
                ddbPaths.push_back(p.string());
            }
        }
    }

    for (const auto &s : ddbPaths){
        std::cout << s << std::endl;
    }
}


}
