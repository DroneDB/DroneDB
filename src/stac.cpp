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
                         const std::string &stacRoot,
                         const std::string &stacEndpoint,
                         const std::string &downloadEndpoint,
                         const std::string &id){
    std::vector<std::string> ddbPaths;

    // Generate list of DDB database paths
    for (const auto &path : paths){
        fs::path p(path);
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

    CURLInstance curl; // for escapeURL

    // What kind of STAC element are we generating?
    if (ddbPaths.size() == 1){
        auto db = open(ddbPaths.front(), false);

        const auto rootId = !id.empty() ? id : fs::weakly_canonical(db->rootDirectory()).filename().string();
        const auto rootTitle = db->getMetaManager()->getString("name", "", "", rootId);

        if (!entry.empty()){
            const auto q = db->query(R"<<<(
                        SELECT path,
                               properties,
                               CASE
                                    WHEN polygon_geom IS NOT NULL THEN AsGeoJSON(polygon_geom)
                                    WHEN point_geom IS NOT NULL THEN AsGeoJSON(point_geom)
                                    ELSE NULL
                               END AS geom,
                               AsWKT(Extent(GUnion(polygon_geom, ConvexHull(point_geom)))) AS bbox
                        FROM entries WHERE path = ?
                    )<<<");
            q->bind(1, entry);
            if (q->fetch()){
                // STAC Item
                const auto path = q->getText(0);
                j["type"] = "Feature";
                j["id"] = path;

                try{
                    j["properties"] = json::parse(q->getText(1), nullptr, false);
                    j["geometry"] = json::parse(q->getText(2), nullptr, false);
                }catch(json::exception &e){
                    throw AppException(std::string("Invalid entry JSON: ") + e.what());
                }

                const auto bbox = wktBboxCoordinates(q->getText(3));
                if (bbox.size() > 0){
                    j["bbox"] = json::array({bbox});
                }else{
                    throw AppException("Cannot compute bbox for STAC item " + entry);
                }

                json links = json::array();

                // Root
                links.push_back({ {"rel", "root"},
                                  {"href", stacRoot + stacEndpoint},
                                  {"type", "application/json"},
                                  {"title", rootTitle}
                                });

                // Self
                if (stacRoot != "."){
                    links.push_back({ {"rel", "self"},
                                      {"href", stacRoot + stacEndpoint + "?path=" + curl.urlEncode(path)},
                                      {"type", "application/geo+json"},
                                      {"title", path}
                                    });
                }

                j["assets"] = json::object();
                j["assets"][path] = {{"href", stacRoot + downloadEndpoint + "?path=" + curl.urlEncode(path)},
                                        {"title", path}};

                j["links"] = links;

            }else{
                throw AppException("Requested STAC entry does not exist");
            }
        }else{
            // Stac Collection
            j["id"] = rootId;
            j["type"] = "Collection";
            j["title"] = rootTitle;

            std::string readme = db->getReadme();
            j["description"] = !readme.empty() ? readme : j["title"].get<std::string>();

            j["license"] = db->getMetaManager()->getString("license", "", "", "proprietary");

            json links = json::array();

            // Root
            links.push_back({ {"rel", "root"},
                              {"href", stacRoot + stacEndpoint},
                              {"type", "application/json"},
                              {"title", j["title"]}
                            });

            // Self
            if (stacRoot != "."){
                links.push_back({ {"rel", "self"},
                                  {"href", stacRoot + stacEndpoint},
                                  {"type", "application/json"},
                                  {"title", j["title"]}
                                });
            }

            // Items
            {
                const auto q = db->query("SELECT path FROM entries WHERE point_geom IS NOT NULL OR polygon_geom IS NOT NULL ORDER BY path");
                while (q->fetch()){
                    const std::string path = q->getText(0);
                    links.push_back({ {"rel", "item"},
                                      {"href", stacRoot + stacEndpoint + "?path=" + curl.urlEncode(path)},
                                      {"type", "application/geo+json"},
                                      {"title", path}
                                    });
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
                    j["assets"][path] = { {"href", stacRoot + downloadEndpoint + "?path=" + curl.urlEncode(path)},
                                          {"title", path} };
                }
            }
        }
    }else if (ddbPaths.size() > 1 && entry.empty()){
        // Stac Catalog
    }else{
        throw AppException("Invalid STAC generation request");
    }

    return j.dump(4);
}

}
