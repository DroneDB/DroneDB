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
#include <cstring>
#include <functional>
#include <limits>
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

        // Gaussian Splats
        if (ext == ".spz") return "application/x-spz";
        if (ext == ".splat") return "application/octet-stream";
        if (ext == ".ksplat") return "application/octet-stream";

        // Video
        if (ext == ".mp4") return "video/mp4";
        if (ext == ".mov") return "video/quicktime";
        if (ext == ".m4v") return "video/x-m4v";
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

    // STAC version and endpoints shared across generators
    static const std::string STAC_VERSION = "1.1.0";
    static const std::string STAC_ENDPOINT = "/stac";
    static const std::string DOWNLOAD_ENDPOINT = "/download";
    static const std::string THUMB_ENDPOINT = "/thumb";

    // Compute a flat [minX, minY, maxX, maxY] bbox from a GeoJSON geometry
    static std::vector<double> bboxFromGeoJson(const json &geom)
    {
        std::vector<double> bbox;
        if (!geom.is_object() || !geom.contains("coordinates"))
            return bbox;

        double minx = std::numeric_limits<double>::max();
        double miny = std::numeric_limits<double>::max();
        double maxx = std::numeric_limits<double>::lowest();
        double maxy = std::numeric_limits<double>::lowest();

        std::function<void(const json &)> walk = [&](const json &c)
        {
            if (!c.is_array())
                return;
            if (c.size() >= 2 && c[0].is_number() && c[1].is_number())
            {
                const double x = c[0].get<double>();
                const double y = c[1].get<double>();
                minx = std::min(minx, x);
                maxx = std::max(maxx, x);
                miny = std::min(miny, y);
                maxy = std::max(maxy, y);
            }
            else
            {
                for (const auto &e : c)
                    walk(e);
            }
        };

        walk(geom["coordinates"]);

        if (minx <= maxx && miny <= maxy)
            bbox = {minx, miny, maxx, maxy};

        return bbox;
    }

    // Parse an ISO 8601 / RFC 3339 instant into epoch seconds (UTC). Returns false on failure.
    // Handles fractional seconds (stripped), Z suffix, and +/-HH:MM timezone offsets.
    static bool parseIso8601ToEpochSecs(const std::string &s, time_t &out)
    {
        if (s.empty())
            return false;

        struct tm tmv;
        std::memset(&tmv, 0, sizeof(tmv));

        std::string base = s;
        int tzOffsetSecs = 0;

        // ISO 8601 / RFC 3339: YYYY-MM-DDTHH:MM:SS[.fff][Z|(+|-)HH:MM]
        const auto tPos = base.find('T');
        if (tPos != std::string::npos)
        {
            // Strip fractional seconds
            const auto dotPos = base.find('.', tPos);
            if (dotPos != std::string::npos)
            {
                const auto fracEnd = base.find_first_of("Z+-", dotPos + 1);
                if (fracEnd == std::string::npos)
                    base.erase(dotPos);
                else
                    base.erase(dotPos, fracEnd - dotPos);
            }

            // Timezone suffix begins no earlier than tPos+9 ("T00:00:00")
            const std::string::size_type minTzPos = tPos + 9;
            if (base.size() > minTzPos)
            {
                const auto zPos = base.find('Z', minTzPos);
                if (zPos != std::string::npos)
                {
                    base.erase(zPos); // UTC - no offset adjustment
                }
                else
                {
                    const auto plusPos  = base.find('+', minTzPos);
                    const auto minusPos = base.find('-', minTzPos);
                    std::string::size_type offPos = std::string::npos;
                    int sign = 0;
                    if (plusPos != std::string::npos)
                    {
                        offPos = plusPos;
                        sign = -1; // +HH:MM ahead of UTC → subtract to get UTC
                    }
                    else if (minusPos != std::string::npos)
                    {
                        offPos = minusPos;
                        sign = +1; // -HH:MM behind UTC → add to get UTC
                    }
                    if (sign != 0)
                    {
                        const std::string tzStr = base.substr(offPos + 1);
                        base.erase(offPos);
                        try
                        {
                            const int h = std::stoi(tzStr.substr(0, 2));
                            const int m = std::stoi(tzStr.size() >= 5 ? tzStr.substr(3, 2) : "0");
                            tzOffsetSecs = sign * (h * 3600 + m * 60);
                        }
                        catch (...) {}
                    }
                }
            }
        }

        std::istringstream ss(base);
        ss >> std::get_time(&tmv, "%Y-%m-%dT%H:%M:%S");
        if (ss.fail())
        {
            std::istringstream ssDate(base);
            ssDate >> std::get_time(&tmv, "%Y-%m-%d");
            if (ssDate.fail())
                return false;
        }

#ifdef _WIN32
        out = _mkgmtime(&tmv);
#else
        out = timegm(&tmv);
#endif
        if (out == static_cast<time_t>(-1))
            return false;

        out += tzOffsetSecs;
        return true;
    }

    // Build a complete STAC Item (Feature) JSON for a single entry row.
    // Shared by the single-item endpoint and the ItemCollection endpoint (DRY).
    static json buildStacItem(const std::string &path,
                              const std::string &propertiesText,
                              const std::string &geomText,
                              int entryTypeInt,
                              long long mtime,
                              const std::string &stacCollectionRoot,
                              const std::string &stacCatalogRoot,
                              const std::string &rootId)
    {
        json j;
        j["stac_version"] = STAC_VERSION;
        j["type"] = "Feature";
        j["id"] = slugify(path, "path-");

        try
        {
            j["properties"] = json::parse(propertiesText, nullptr, false);
            // Add title to properties for better display in STAC browsers
            j["properties"]["title"] = path;
            j["geometry"] = json::parse(geomText, nullptr, false);
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
            else if (mtime > 0)
            {
                // Fallback to mtime (filesystem modification time)
                j["properties"]["datetime"] = epochSecsToIso8601(static_cast<time_t>(mtime));
            }
            else
            {
                j["properties"]["datetime"] = j_null;
            }
        }

        // Projection STAC extension
        {
            const bool hasProjection = j["properties"].contains("geotransform") &&
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
                    j["properties"]["proj:shape"] = json::array({j["properties"]["height"],
                                                                 j["properties"]["width"]});
                    j["properties"].erase("height");
                    j["properties"].erase("width");
                }

                // proj:wkt2 (from projection)
                const std::string wkt = j["properties"]["projection"].get<std::string>();
                j["properties"]["proj:wkt2"] = wkt;
                j["properties"].erase("projection");

                // proj:code (Projection Extension v2.0.0 replaced proj:epsg)
                const int epsg = extractEpsgFromWkt(wkt);
                if (epsg > 0)
                    j["properties"]["proj:code"] = "EPSG:" + std::to_string(epsg);
            }
        }

        const auto bbox = bboxFromGeoJson(j["geometry"]);
        if (!bbox.empty())
            j["bbox"] = bbox;

        json links = json::array();

        // Root
        if (!stacCatalogRoot.empty())
        {
            links.push_back({{"rel", "root"},
                             {"href", stacCatalogRoot + STAC_ENDPOINT},
                             {"type", "application/json"}});
        }

        if (stacCollectionRoot != ".")
        {
            // Parent
            links.push_back({{"rel", "parent"},
                             {"href", stacCollectionRoot + STAC_ENDPOINT},
                             {"type", "application/json"}});

            // Collection
            links.push_back({{"rel", "collection"},
                             {"href", stacCollectionRoot + STAC_ENDPOINT},
                             {"type", "application/json"}});

            // Self
            links.push_back({{"rel", "self"},
                             {"href", stacCollectionRoot + STAC_ENDPOINT + "/" + Base64::encode(path)},
                             {"type", "application/geo+json"}});

            // STAC Item schema requires the top-level "collection" field whenever a
            // rel:collection link is present; its value must match the Collection id.
            j["collection"] = rootId;
        }

        j["assets"] = json::object();
        j["assets"][path] = {{"href", stacCollectionRoot + DOWNLOAD_ENDPOINT + "?path=" + std::string(cpr::util::urlEncode(path))},
                             {"title", path},
                             {"type", getStacMimeType(path)},
                             {"roles", json::array({"data"})}};

        const EntryType t = static_cast<EntryType>(entryTypeInt);
        if (supportsThumbnails(t) || t == PointCloud || t == GaussianSplat)
        {
            j["assets"]["thumbnail"] = {{"title", "Thumbnail"},
                                        {"type", "image/jpeg"},
                                        {"roles", json::array({"thumbnail"})},
                                        {"href", stacCollectionRoot + THUMB_ENDPOINT + "?path=" + std::string(cpr::util::urlEncode(path)) + "&size=512"}};
        }

        j["links"] = links;
        return j;
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
        j["stac_version"] = STAC_VERSION;

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
                           type,
                           mtime
                    FROM entries WHERE path = ?
                )<<<");
            q->bind(1, entry);
            if (q->fetch())
            {
                // STAC Item (delegates to the shared builder)
                j = buildStacItem(q->getText(0), q->getText(1), q->getText(2),
                                  q->getInt(3), q->getInt64(4),
                                  stacCollectionRoot, stacCatalogRoot, rootId);
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

    json generateStacItemCollection(const std::string &ddbPath,
                                    const std::string &stacCollectionRoot,
                                    const std::string &id,
                                    const std::string &stacCatalogRoot,
                                    const std::vector<double> &bbox,
                                    const std::string &datetimeStart,
                                    const std::string &datetimeEnd,
                                    int limit,
                                    int offset)
    {
        if (ddbPath.empty())
            throw AppException("No ddbPath is set for generating STAC");

        if (limit <= 0)
            limit = 10;
        if (offset < 0)
            offset = 0;

        auto db = open(ddbPath, false);
        const auto rootId = !id.empty() ? id : fs::weakly_canonical(db->rootDirectory()).filename().string();

        // High-precision double formatter for inlining bbox literals (bind() has no double overload)
        const auto fmtD = [](double v)
        {
            std::ostringstream o;
            o << std::setprecision(15) << v;
            return o.str();
        };

        // Shared WHERE clause for the data and count queries
        std::string where = "(polygon_geom IS NOT NULL OR point_geom IS NOT NULL)";

        const bool hasBbox = bbox.size() == 4;
        if (hasBbox)
        {
            // Geometries are stored in EPSG:4326; intersect against the request MBR
            where += " AND MbrIntersects(BuildMbr(" + fmtD(bbox[0]) + ", " + fmtD(bbox[1]) +
                     ", " + fmtD(bbox[2]) + ", " + fmtD(bbox[3]) + ", 4326), "
                     "CASE WHEN polygon_geom IS NOT NULL THEN polygon_geom ELSE point_geom END)";
        }

        time_t dtStart = 0, dtEnd = 0;
        const bool hasStart = parseIso8601ToEpochSecs(datetimeStart, dtStart);
        const bool hasEnd = parseIso8601ToEpochSecs(datetimeEnd, dtEnd);

        // Per-entry instant (seconds): captureTime (ms) if present and > 0, else mtime
        const std::string instantExpr =
            "(CASE WHEN json_extract(properties, '$.captureTime') IS NOT NULL "
            "AND json_extract(properties, '$.captureTime') > 0 "
            "THEN json_extract(properties, '$.captureTime') / 1000.0 ELSE mtime END)";

        if (hasStart)
            where += " AND " + instantExpr + " >= ?";
        if (hasEnd)
            where += " AND " + instantExpr + " <= ?";

        // numberMatched (total entries matching the filters, ignoring paging)
        long long numberMatched = 0;
        {
            const auto cq = db->query("SELECT COUNT(*) FROM entries WHERE " + where);
            int idx = 1;
            if (hasStart)
                cq->bind(idx++, static_cast<long long>(dtStart));
            if (hasEnd)
                cq->bind(idx++, static_cast<long long>(dtEnd));
            if (cq->fetch())
                numberMatched = cq->getInt64(0);
        }

        const std::string sql =
            "SELECT path, properties, "
            "CASE WHEN polygon_geom IS NOT NULL THEN AsGeoJSON(polygon_geom) "
            "WHEN point_geom IS NOT NULL THEN AsGeoJSON(point_geom) ELSE NULL END AS geom, "
            "type, mtime FROM entries WHERE " +
            where + " ORDER BY path LIMIT ? OFFSET ?";

        const auto q = db->query(sql);
        int idx = 1;
        if (hasStart)
            q->bind(idx++, static_cast<long long>(dtStart));
        if (hasEnd)
            q->bind(idx++, static_cast<long long>(dtEnd));
        q->bind(idx++, static_cast<long long>(limit));
        q->bind(idx++, static_cast<long long>(offset));

        json features = json::array();
        while (q->fetch())
        {
            features.push_back(buildStacItem(q->getText(0), q->getText(1), q->getText(2),
                                             q->getInt(3), q->getInt64(4),
                                             stacCollectionRoot, stacCatalogRoot, rootId));
        }

        json fc;
        fc["type"] = "FeatureCollection";
        fc["features"] = features;
        fc["numberReturned"] = features.size();
        fc["numberMatched"] = numberMatched;

        json links = json::array();
        if (!stacCatalogRoot.empty())
            links.push_back({{"rel", "root"},
                             {"href", stacCatalogRoot + STAC_ENDPOINT},
                             {"type", "application/json"}});
        if (stacCollectionRoot != ".")
        {
            links.push_back({{"rel", "self"},
                             {"href", stacCollectionRoot + STAC_ENDPOINT + "/items"},
                             {"type", "application/geo+json"}});
            links.push_back({{"rel", "collection"},
                             {"href", stacCollectionRoot + STAC_ENDPOINT},
                             {"type", "application/json"}});
        }
        fc["links"] = links;

        return fc;
    }

}
