/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "stac.h"
#include "logger.h"
#include "exceptions.h"
#include "mio.h"
#include "curl_inc.h"
#include "ddb.h"

namespace ddb{

std::string generateStac(const std::vector<std::string> &paths,
                         const std::string &entry,
                         const std::string &matchExpr,
                         bool recursive,
                         int maxRecursionDepth,
                         const std::string &stacEndpoint,
                         const std::string &downloadEndpoint,
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

        std::string readme = db->getReadme();
        j["description"] = !readme.empty() ? readme : j["title"].get<std::string>();

        j["license"] = db->getMetaManager()->getString("license", "", "", "proprietary");

        json links = json::array();

        // Root
        links.push_back({ {"rel", "root"},
                          {"href", stacEndpoint},
                          {"type", "application/json"},
                          {"title", j["title"]}
                        });
        CURL *curl = curl_easy_init();
        if(!curl) throw AppException("Cannot initialize CURL");

        // Items
        {
            const auto q = db->query("SELECT path FROM entries WHERE point_geom IS NOT NULL OR polygon_geom IS NOT NULL ORDER BY path");
            while (q->fetch()){
                const std::string path = q->getText(0);

                char *escapedPath = curl_easy_escape(curl, path.c_str(), path.size());
                if(escapedPath) {
                    links.push_back({ {"rel", "item"},
                                      {"href", stacEndpoint + "?p=" + std::string(escapedPath)},
                                      {"type", "application/geo+json"},
                                      {"title", path}
                                    });
                    curl_free(escapedPath);
                }
            }
        }

        // Self? It's strongly recommended, but then we need to use absolute URLs..

        j["links"] = links;
        j["extent"] = db->getExtent();

        j["assets"] = json::object();

        // Assets
        {
            const auto q = db->query("SELECT path FROM entries WHERE point_geom IS NULL AND polygon_geom IS NULL "
                                     "AND type != 1 AND type != 7 ORDER BY path");
            while (q->fetch()){
                const std::string path = q->getText(0);

                char *escapedPath = curl_easy_escape(curl, path.c_str(), path.size());
                if(escapedPath) {
                    j["assets"][path] = { {"href", downloadEndpoint + "?p=" + std::string(escapedPath)},
                                      {"title", path}
                                    };
                    curl_free(escapedPath);
                }
            }
        }

        curl_easy_cleanup(curl);
    }else if (ddbPaths.size() > 1 && entry.empty()){
        // Stac Catalog
    }else{
        throw AppException("Invalid STAC generation request");
    }

    return j.dump(4);
}

}
