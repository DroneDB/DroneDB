/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "stac.h"
#include "logger.h"
#include "exceptions.h"
#include "mio.h"
#include <cpr/cpr.h>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cctype>
#include "ddb.h"
#include "thumbs.h"
#include "../../vendor/base64/base64.h"

namespace ddb
{

    // Convert epoch milliseconds (double) to ISO 8601 UTC string
    static std::string epochMsToIso8601(double epochMs)
    {
        time_t secs = static_cast<time_t>(epochMs / 1000.0);
        struct tm utcTime;
#ifdef _WIN32
        gmtime_s(&utcTime, &secs);
#else
        gmtime_r(&secs, &utcTime);
#endif
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utcTime);
        return std::string(buf);
    }

    // Convert epoch seconds (time_t) to ISO 8601 UTC string
    static std::string epochSecsToIso8601(time_t epochSecs)
    {
        struct tm utcTime;
#ifdef _WIN32
        gmtime_s(&utcTime, &epochSecs);
#else
        gmtime_r(&epochSecs, &utcTime);
#endif
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utcTime);
        return std::string(buf);
    }

    // Slugify a string to conform to STAC best practices: lowercase [a-z0-9_-]
    static std::string slugify(const std::string &input, const std::string &prefix = "")
    {
        std::string result;
        result.reserve(prefix.size() + input.size());
        if (!prefix.empty())
        {
            result += prefix;
        }
        for (char ch : input)
        {
            char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (std::isalnum(static_cast<unsigned char>(lower)))
            {
                result += lower;
            }
            else if (lower == '_')
            {
                result += '_';
            }
            else
            {
                // Replace spaces, dots, and other chars with '-'
                if (!result.empty() && result.back() != '-')
                    result += '-';
            }
        }
        // Trim leading/trailing dashes
        while (!result.empty() && result.front() == '-') result.erase(result.begin());
        while (!result.empty() && result.back() == '-') result.pop_back();
        return result;
    }

    // Get MIME type from file extension for STAC assets
    static std::string getStacMimeType(const std::string &path)
    {
        auto ext = fs::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        // Images
        if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
        if (ext == ".tif" || ext == ".tiff") return "image/tiff";
        if (ext == ".png") return "image/png";
        if (ext == ".webp") return "image/webp";
        if (ext == ".gif") return "image/gif";
        if (ext == ".bmp") return "image/bmp";
        if (ext == ".svg") return "image/svg+xml";
        if (ext == ".dng") return "image/tiff";
        if (ext == ".ico") return "image/x-icon";

        // Point clouds
        if (ext == ".laz") return "application/vnd.laszip+copc";
        if (ext == ".las") return "application/vnd.las";
        if (ext == ".e57") return "application/x-e57";
        if (ext == ".pts") return "application/x-pts";
        if (ext == ".xyz") return "text/plain";
        if (ext == ".ply") return "application/x-ply";
        if (ext == ".pcd") return "application/x-pcd";

        // Video
        if (ext == ".mp4") return "video/mp4";
        if (ext == ".mov") return "video/quicktime";
        if (ext == ".avi") return "video/x-msvideo";
        if (ext == ".mkv") return "video/x-matroska";
        if (ext == ".webm") return "video/webm";
        if (ext == ".wmv") return "video/x-ms-wmv";
        if (ext == ".flv") return "video/x-flv";

        // 3D models
        if (ext == ".obj") return "model/obj";
        if (ext == ".gltf") return "model/gltf+json";
        if (ext == ".glb") return "model/gltf-binary";
        if (ext == ".stl") return "model/stl";
        if (ext == ".dae") return "model/vnd.collada+xml";
        if (ext == ".3ds") return "application/x-3ds";
        if (ext == ".fbx") return "application/x-fbx";
        if (ext == ".nxs" || ext == ".nxz") return "application/octet-stream";
        if (ext == ".mtl") return "model/mtl";

        // Vector / GIS
        if (ext == ".geojson") return "application/geo+json";
        if (ext == ".gpkg") return "application/geopackage+sqlite3";
        if (ext == ".fgb" || ext == ".flatgeobuf") return "application/flatgeobuf";
        if (ext == ".kml") return "application/vnd.google-earth.kml+xml";
        if (ext == ".kmz") return "application/vnd.google-earth.kmz";
        if (ext == ".shp" || ext == ".shz") return "application/x-shapefile";
        if (ext == ".shx" || ext == ".dbf" || ext == ".prj") return "application/x-shapefile";
        if (ext == ".dxf") return "application/dxf";
        if (ext == ".dwg") return "application/x-dwg";
        if (ext == ".topojson") return "application/json";
        if (ext == ".gpx") return "application/gpx+xml";
        if (ext == ".gml") return "application/gml+xml";
        if (ext == ".wkt") return "text/plain";

        // Documents / text
        if (ext == ".md") return "text/markdown";
        if (ext == ".pdf") return "application/pdf";
        if (ext == ".txt") return "text/plain";
        if (ext == ".csv") return "text/csv";
        if (ext == ".json") return "application/json";
        if (ext == ".xml") return "application/xml";
        if (ext == ".html" || ext == ".htm") return "text/html";
        if (ext == ".yaml" || ext == ".yml") return "text/yaml";

        // Archives
        if (ext == ".zip") return "application/zip";
        if (ext == ".gz" || ext == ".gzip") return "application/gzip";
        if (ext == ".tar") return "application/x-tar";
        if (ext == ".7z") return "application/x-7z-compressed";

        return "application/octet-stream";
    }

    // Try to extract EPSG code from a WKT projection string
    static int extractEpsgFromWkt(const std::string &wkt)
    {
        // Look for AUTHORITY["EPSG","NNNNN"] pattern in the WKT
        // Use string search instead of regex for MSVC compatibility
        const std::string marker = "AUTHORITY[\"EPSG\",\"";
        auto pos = wkt.rfind(marker);
        if (pos != std::string::npos)
        {
            pos += marker.size();
            auto end = wkt.find('"', pos);
            if (end != std::string::npos && end > pos)
            {
                try
                {
                    return std::stoi(wkt.substr(pos, end - pos));
                }
                catch (...)
                {
                    return 0;
                }
            }
        }
        return 0;
    }

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
                           type,
                           mtime
                    FROM entries WHERE path = ?
                )<<<");
            q->bind(1, entry);
            if (q->fetch())
            {
                // STAC Item
                const auto path = q->getText(0);
                j["type"] = "Feature";
                j["id"] = slugify(path, "path-");

                try
                {
                    j["properties"] = json::parse(q->getText(1), nullptr, false);
                    // Add title to properties for better display in STAC browsers
                    j["properties"]["title"] = path;
                    j["geometry"] = json::parse(q->getText(2), nullptr, false);
                }
                catch (json::exception &e)
                {
                    throw AppException(std::string("Invalid entry JSON: ") + e.what());
                }

                //  STAC requires datetime property
                {
                    json j_null;
                    if (j["properties"].contains("captureTime") &&
                        j["properties"]["captureTime"].is_number() &&
                        j["properties"]["captureTime"].get<double>() > 0)
                    {
                        j["properties"]["datetime"] = epochMsToIso8601(j["properties"]["captureTime"].get<double>());
                    }
                    else
                    {
                        // Fallback to mtime (filesystem modification time)
                        time_t mtime = static_cast<time_t>(q->getInt64(5));
                        if (mtime > 0)
                        {
                            j["properties"]["datetime"] = epochSecsToIso8601(mtime);
                        }
                        else
                        {
                            j["properties"]["datetime"] = j_null;
                        }
                    }
                }

                // Projection STAC extension
                {
                    bool hasProjection = j["properties"].contains("geotransform") &&
                                        j["properties"].contains("projection");
                    if (hasProjection)
                    {
                        json stacExtensions = json::array();
                        stacExtensions.push_back("https://stac-extensions.github.io/projection/v2.0.0/schema.json");
                        j["stac_extensions"] = stacExtensions;

                        // proj:transform (from geotransform)
                        j["properties"]["proj:transform"] = j["properties"]["geotransform"];
                        j["properties"].erase("geotransform");

                        // proj:shape [height, width] (rows, cols)
                        if (j["properties"].contains("height") && j["properties"].contains("width"))
                        {
                            j["properties"]["proj:shape"] = json::array({
                                j["properties"]["height"],
                                j["properties"]["width"]
                            });
                            j["properties"].erase("height");
                            j["properties"].erase("width");
                        }

                        // proj:wkt2 (from projection)
                        std::string wkt = j["properties"]["projection"].get<std::string>();
                        j["properties"]["proj:wkt2"] = wkt;
                        j["properties"].erase("projection");

                        // Try to extract EPSG code
                        int epsg = extractEpsgFromWkt(wkt);
                        if (epsg > 0)
                        {
                            j["properties"]["proj:epsg"] = epsg;
                        }
                    }
                }

                const auto bbox = wktBboxCoordinates(q->getText(3));
                if (bbox.size() > 0)
                {
                    j["bbox"] = bbox;
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
                j["assets"][path] = {{"href", stacCollectionRoot + downloadEndpoint + "?path=" + std::string(cpr::util::urlEncode(path))},
                                     {"title", path},
                                     {"type", getStacMimeType(path)},
                                     {"roles", json::array({"data"})}};

                EntryType t = static_cast<EntryType>(q->getInt(4));
                if (supportsThumbnails(t) || t == PointCloud)
                {
                    j["assets"]["thumbnail"] = {{"title", "Thumbnail"},
                                                {"type", "image/jpeg"},
                                                {"roles", json::array({"thumbnail"})},
                                                {"href", stacCollectionRoot + thumbEndpoint + "?path=" + std::string(cpr::util::urlEncode(path)) + "&size=512"}};
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
                    const auto href = stacCollectionRoot + downloadEndpoint + "?path=" + std::string(cpr::util::urlEncode(path));
                    j["assets"][path] = {{"href", href},
                                         {"title", path}};
                }
            }
        }

        return j;
    }

}
