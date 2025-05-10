/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "stac.h"
#include "logger.h"
#include "exceptions.h"
#include "mio.h"
#include <cpr/cpr.h>
#include "ddb.h"
#include "thumbs.h"
#include "../../vendor/base64/base64.h"

namespace ddb
{

    json generateStac(const std::string &ddbPath,
                      const std::string &entry,
                      const std::string &stacCollectionRoot,
                      const std::string &id,
                      const std::string &stacCatalogRoot)
    {
        // Collection -> Dataset STAC
        // Catalog -> Entry point STAC / root (index of multiple Collection)

        const std::string stacEndpoint = "/stac";
        const std::string downloadEndpoint = "/download";
        const std::string thumbEndpoint = "/thumb";

        if (ddbPath.empty())
            throw AppException("No ddbPath is set for generating STAC");

        json j;
        j["stac_version"] = "1.0.0";

        //CURLInstance curl; // for urlEncode

        // What kind of STAC element are we generating?
        auto db = open(ddbPath, false);

        const auto rootId = !id.empty() ? id : fs::weakly_canonical(db->rootDirectory()).filename().string();
        const auto rootTitle = db->getMetaManager()->getString("name", "", "", rootId);

        if (!entry.empty())
        {
            const auto q = db->query(R"<<<(
                    SELECT path,
                           properties,
                           CASE
                                WHEN polygon_geom IS NOT NULL THEN AsGeoJSON(polygon_geom)
                                WHEN point_geom IS NOT NULL THEN AsGeoJSON(point_geom)
                                ELSE NULL
                           END AS geom,
                           AsWKT(Extent(GUnion(polygon_geom, ConvexHull(point_geom)))) AS bbox,
                           type
                    FROM entries WHERE path = ?
                )<<<");
            q->bind(1, entry);
            if (q->fetch())
            {
                // STAC Item
                const auto path = q->getText(0);
                j["type"] = "Feature";
                j["id"] = path;

                try
                {
                    j["properties"] = json::parse(q->getText(1), nullptr, false);
                    j["geometry"] = json::parse(q->getText(2), nullptr, false);
                }
                catch (json::exception &e)
                {
                    throw AppException(std::string("Invalid entry JSON: ") + e.what());
                }

                const auto bbox = wktBboxCoordinates(q->getText(3));
                if (bbox.size() > 0)
                {
                    j["bbox"] = json::array({bbox});
                }

                json links = json::array();

                // Root
                if (!stacCatalogRoot.empty())
                {
                    links.push_back({{"rel", "root"},
                                     {"href", stacCatalogRoot + stacEndpoint},
                                     {"type", "application/json"}});
                }

                if (stacCollectionRoot != ".")
                {
                    // Parent
                    links.push_back({{"rel", "parent"},
                                     {"href", stacCollectionRoot + stacEndpoint},
                                     {"type", "application/json"}});

                    // CollectionstacCollectionRoot
                    links.push_back({{"rel", "collection"},
                                     {"href", stacCollectionRoot + stacEndpoint},
                                     {"type", "application/json"}});

                    // Self
                    links.push_back({{"rel", "self"},
                                     {"href", stacCollectionRoot + stacEndpoint + "/" + Base64::encode(path)},
                                     {"type", "application/geo+json"}});
                }

                j["assets"] = json::object();
                j["assets"][path] = {{"href", stacCollectionRoot + downloadEndpoint + "/" + path},
                                     {"title", path}};

                EntryType t = static_cast<EntryType>(q->getInt(4));
                if (supportsThumbnails(t) || t == PointCloud)
                {
                    j["assets"]["thumbnail"] = {{"title", "Thumbnail"},
                                                {"type", "image/jpeg"},
                                                {"roles", json::array({"thumbnail"})},
                                                {"href", stacCollectionRoot + thumbEndpoint + "?path=" + cpr::util::urlEncode(path) + "&size=512"}};
                }
                j["links"] = links;
            }
            else
            {
                throw AppException("Requested STAC entry does not exist");
            }
        }
        else
        {
            // Stac Collection
            j["id"] = rootId;
            j["type"] = "Collection";
            j["title"] = rootTitle;

            std::string readme = db->getReadme();
            j["description"] = !readme.empty() ? readme : j["title"].get<std::string>();

            j["license"] = db->getMetaManager()->getString("license", "", "", "proprietary");

            json links = json::array();

            // Root
            if (!stacCatalogRoot.empty())
            {
                links.push_back({{"rel", "root"},
                                 {"href", stacCatalogRoot + stacEndpoint},
                                 {"type", "application/json"}});
                // Parent
                links.push_back({{"rel", "parent"},
                                 {"href", stacCatalogRoot + stacEndpoint},
                                 {"type", "application/json"}});
            }

            if (stacCollectionRoot != ".")
            {
                // Self
                links.push_back({{"rel", "self"},
                                 {"href", stacCollectionRoot + stacEndpoint},
                                 {"type", "application/json"}});
            }

            // Items
            {
                const auto q = db->query("SELECT path FROM entries WHERE point_geom IS NOT NULL OR polygon_geom IS NOT NULL ORDER BY path");
                while (q->fetch())
                {
                    const std::string path = q->getText(0);
                    links.push_back({{"rel", "item"},
                                     {"href", stacCollectionRoot + stacEndpoint + "/" + Base64::encode(path)},
                                     {"type", "application/geo+json"},
                                     {"title", path}});
                }
            }

            j["links"] = links;
            j["extent"] = db->getExtent();

            j["assets"] = json::object();

            // Assets
            {
                const auto q = db->query("SELECT path FROM entries WHERE point_geom IS NULL AND polygon_geom IS NULL "
                                         "AND type != 1 AND type != 7 ORDER BY path");
                while (q->fetch())
                {
                    const std::string path = q->getText(0);
                    j["assets"][path] = {{"href", stacCollectionRoot + downloadEndpoint + "?path=" + cpr::util::urlEncode(path)},
                                         {"title", path}};
                }
            }
        }

        return j;
    }

}
