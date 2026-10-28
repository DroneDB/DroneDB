/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "stac.h"
#include "index_sql.h"
#include "json.h"
#include "stringutil.h"
#include "timeutil.h"
#include "logger.h"
#include "exceptions.h"
#include "mio.h"
#include "http_client.h"
#include <ctime>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <limits>
#include "build_layout.h"
#include "ddb.h"
#include "entry_handler.h"
#include "../../vendor/base64/base64.h"

namespace ddb
{

    // Epoch milliseconds (double) as an ISO 8601 UTC string
    static std::string epochMsToIso8601(double epochMs)
    {
        return utcIso8601(static_cast<time_t>(epochMs / 1000.0));
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
            const char lower = str::toLowerChar(ch);
            if (str::isAlnumChar(lower))
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
        // With the dot, as the table below spells its extensions
        const auto ext = "." + io::lowerExtension(io::fromUtf8(path));

        // Images
        if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
        if (ext == ".tif" || ext == ".tiff") return "image/tiff";
        if (ext == ".jp2") return "image/jp2";
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
        // The type the build manifest names for the files of a splat
        if (ext == ".spz" || ext == ".splat" || ext == ".ksplat") return GsplatMediaType;

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
        if (ext == ".bm3d" || ext == ".nxs" || ext == ".nxz")
            return "application/octet-stream";
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

    // The Registry routes that the STAC outputs of DDBStac and DDBStacItemCollection link to.
    // The data-only outputs (DDBStacData, DDBStacItemCollectionData) have none: they
    // leave out every member built from a URL or an ID ("id", "collection", "links" and the
    // asset "href"s), which the caller (Registry) owns.
    struct StacUrls {
        const std::string& collectionRoot;
        const std::string& catalogRoot;
    };

    // The STAC id of the dataset: `id`, else the name of the dataset folder
    static std::string datasetRootId(Database* db, const std::string& id) {
        return !id.empty() ? id : io::toUtf8(fs::weakly_canonical(db->rootDirectory()).filename());
    }

    // The columns of an entry row a STAC Item is built from (the SELECT of stacItemSql)
    struct StacRow {
        std::string path;
        std::string propertiesText;
        std::string geomText;
        int entryTypeInt = 0;
        long long mtime = 0;
    };

    // path, properties, the GeoJSON of the footprint, type and mtime, in the order StacRow reads them
    static const char* const stacRowColumns =
        "path, properties, "
        "CASE WHEN polygon_geom IS NOT NULL THEN AsGeoJSON(polygon_geom) "
        "WHEN point_geom IS NOT NULL THEN AsGeoJSON(point_geom) ELSE NULL END AS geom, "
        "type, mtime";

    static StacRow readStacRow(Statement& q) {
        return {q.getText(0), q.getText(1), q.getText(2), q.getInt(3), q.getInt64(4)};
    }

    // A link: rel, href and type (and a title when it has one)
    static json stacLink(const char* rel, const std::string& href, const char* type, const std::string& title = "") {
        json link = {{"rel", rel}, {"href", href}, {"type", type}};
        if (!title.empty())
            link["title"] = title;
        return link;
    }

    // The properties, title and geometry of the Item of a row
    static void setItemContent(json& j, const StacRow& row) {
        try
        {
            j["properties"] = json::parse(row.propertiesText, nullptr, false);
            // Add title to properties for better display in STAC browsers
            j["properties"]["title"] = row.path;
            j["geometry"] = json::parse(row.geomText, nullptr, false);
        }
        catch (json::exception &e)
        {
            throw AppException(std::string("Invalid entry JSON: ") + e.what());
        }
    }

    // STAC requires the datetime property: the capture time, else the modification time
    static void setItemDatetime(json& properties, long long mtime) {
        if (properties.contains("captureTime") && properties["captureTime"].is_number() &&
            properties["captureTime"].get<double>() > 0)
        {
            properties["datetime"] = epochMsToIso8601(properties["captureTime"].get<double>());
        }
        else if (mtime > 0)
        {
            // Fallback to mtime (filesystem modification time)
            properties["datetime"] = utcIso8601(static_cast<time_t>(mtime));
        }
        else
        {
            properties["datetime"] = json();
        }
    }

    // Projection STAC extension: a raster's geotransform, size and projection under their proj: names
    static void applyProjectionExtension(json& j) {
        json& properties = j["properties"];
        if (!(properties.contains("geotransform") && properties.contains("projection")))
            return;

        j["stac_extensions"] = json::array({"https://stac-extensions.github.io/projection/v2.0.0/schema.json"});

        // proj:transform (from geotransform)
        properties["proj:transform"] = properties["geotransform"];
        properties.erase("geotransform");

        // proj:shape [height, width] (rows, cols)
        if (properties.contains("height") && properties.contains("width"))
        {
            properties["proj:shape"] = json::array({properties["height"], properties["width"]});
            properties.erase("height");
            properties.erase("width");
        }

        // proj:wkt2 (from projection)
        const std::string wkt = properties["projection"].get<std::string>();
        properties["proj:wkt2"] = wkt;
        properties.erase("projection");

        // proj:code (Projection Extension v2.0.0 replaced proj:epsg)
        const int epsg = extractEpsgFromWkt(wkt);
        if (epsg > 0)
            properties["proj:code"] = "EPSG:" + std::to_string(epsg);
    }

    // The links of an Item (and its collection id)
    static void setItemLinks(json& j, const StacUrls& urls, const std::string& path, const std::string& rootId) {
        json links = json::array();

        // Root
        if (!urls.catalogRoot.empty())
            links.push_back(stacLink("root", urls.catalogRoot + STAC_ENDPOINT, "application/json"));

        if (urls.collectionRoot != ".") {
            const std::string collection = urls.collectionRoot + STAC_ENDPOINT;
            links.push_back(stacLink("parent", collection, "application/json"));
            links.push_back(stacLink("collection", collection, "application/json"));
            links.push_back(stacLink("self", collection + "/" + Base64::encode(path), "application/geo+json"));

            // STAC Item schema requires the top-level "collection" field whenever a
            // rel:collection link is present; its value must match the Collection id.
            j["collection"] = rootId;
        }

        j["links"] = links;
    }

    // The data asset of an Item, and its thumbnail when one can be rendered
    static void setItemAssets(json& j, const StacRow& row, const StacUrls* urls) {
        const std::string& path = row.path;
        const std::string pathQuery = urls != nullptr ? "?path=" + urlEncode(path) : "";

        j["assets"] = json::object();
        j["assets"][path] = {{"title", path},
                             {"type", getStacMimeType(path)},
                             {"roles", json::array({"data"})}};
        if (urls != nullptr)
            j["assets"][path]["href"] = urls->collectionRoot + DOWNLOAD_ENDPOINT + pathQuery;

        // Rendered from the file itself or from a build artifact (COPC, RAD)
        const EntryType t = static_cast<EntryType>(row.entryTypeInt);
        if (entryHandler(t).thumbnailSource() != ThumbnailSource::None)
        {
            j["assets"]["thumbnail"] = {{"title", "Thumbnail"},
                                        {"type", "image/webp"},
                                        {"roles", json::array({"thumbnail"})}};
            if (urls != nullptr)
                j["assets"]["thumbnail"]["href"] =
                    urls->collectionRoot + THUMB_ENDPOINT + pathQuery + "&size=512";
        }
    }

    // Build a complete STAC Item (Feature) JSON for a single entry row.
    // Shared by the single-item endpoint and the ItemCollection endpoint (DRY).
    // urls: null for the data-only item (no id, collection, links or asset hrefs)
    static json buildStacItem(const StacRow& row, const StacUrls* urls, const std::string& rootId) {
        json j;
        j["stac_version"] = STAC_VERSION;
        j["type"] = "Feature";
        if (urls != nullptr)
            j["id"] = slugify(row.path, "path-");

        setItemContent(j, row);
        setItemDatetime(j["properties"], row.mtime);
        applyProjectionExtension(j);

        const auto bbox = bboxFromGeoJson(j["geometry"]);
        if (!bbox.empty())
            j["bbox"] = bbox;

        if (urls != nullptr)
            setItemLinks(j, *urls, row.path, rootId);
        setItemAssets(j, row, urls);
        return j;
    }

    // The STAC Item of `entry`
    static json stacItem(Database* db, const std::string& entry, const StacUrls* urls, const std::string& rootId) {
        const auto q = db->query(std::string("SELECT ") + stacRowColumns + " FROM entries WHERE path = ?");
        q->bind(1, entry);
        if (!q->fetch())
            throw EntryNotFoundException("Requested STAC entry does not exist");
        return buildStacItem(readStacRow(*q), urls, rootId);
    }

    // The links of the Collection of the dataset: its catalog, itself and an item per entry with a geometry
    static json collectionLinks(Database* db, const StacUrls& urls) {
        json links = json::array();

        // Root
        if (!urls.catalogRoot.empty()) {
            links.push_back(stacLink("root", urls.catalogRoot + STAC_ENDPOINT, "application/json"));
            links.push_back(stacLink("parent", urls.catalogRoot + STAC_ENDPOINT, "application/json"));
        }

        if (urls.collectionRoot != ".")
            links.push_back(stacLink("self", urls.collectionRoot + STAC_ENDPOINT, "application/json"));

        // Items
        const auto q = db->query(
            "SELECT path FROM entries WHERE point_geom IS NOT NULL OR polygon_geom IS "
            "NOT NULL ORDER BY path");
        while (q->fetch()) {
            const std::string path = q->getText(0);
            links.push_back(stacLink("item", urls.collectionRoot + STAC_ENDPOINT + "/" + Base64::encode(path),
                                     "application/geo+json", path));
        }
        return links;
    }

    // The assets of the Collection: the entries without a geometry, but folders and the dataset itself
    static json collectionAssets(Database* db, const StacUrls* urls) {
        json assets = json::object();
        const auto q = db->query("SELECT path FROM entries WHERE point_geom IS NULL AND polygon_geom IS NULL AND " +
                                 typeCondition("type", "!=", EntryType::Directory) + " AND " +
                                 typeCondition("type", "!=", EntryType::DroneDB) + " ORDER BY path");
        while (q->fetch())
        {
            const std::string path = q->getText(0);
            assets[path] = {{"title", path}};
            if (urls != nullptr)
                assets[path]["href"] = urls->collectionRoot + DOWNLOAD_ENDPOINT + "?path=" + urlEncode(path);
        }
        return assets;
    }

    // The STAC Collection of the dataset
    static json stacCollection(Database* db, const StacUrls* urls, const std::string& rootId) {
        json j;
        j["stac_version"] = STAC_VERSION;
        if (urls != nullptr)
            j["id"] = rootId;
        j["type"] = "Collection";
        j["title"] = db->getMetaManager()->getString("name", "", "", rootId);

        std::string readme = db->getReadme();
        j["description"] = !readme.empty() ? readme : j["title"].get<std::string>();

        j["license"] = db->getMetaManager()->getString("license", "", "", "proprietary");

        if (urls != nullptr)
            j["links"] = collectionLinks(db, *urls);

        j["extent"] = db->getExtent();
        j["assets"] = collectionAssets(db, urls);
        return j;
    }

    // The STAC Item of `entry`, or the Collection of the dataset when entry is ""; urls: null
    // for the data-only document
    static json generateStacDocument(const std::string& ddbPath,
                                     const std::string& entry,
                                     const std::string& id,
                                     const StacUrls* urls) {
        // Collection -> Dataset STAC
        // Catalog -> Entry point STAC / root (index of multiple Collection)

        if (ddbPath.empty())
            throw AppException("No ddbPath is set for generating STAC");

        // What kind of STAC element are we generating?
        auto db = open(ddbPath, false);
        const auto rootId = datasetRootId(db.get(), id);
        return !entry.empty() ? stacItem(db.get(), entry, urls, rootId) : stacCollection(db.get(), urls, rootId);
    }

    json generateStac(const std::string& ddbPath,
                      const std::string& entry,
                      const std::string& stacCollectionRoot,
                      const std::string& id,
                      const std::string& stacCatalogRoot) {
        const StacUrls urls{stacCollectionRoot, stacCatalogRoot};
        return generateStacDocument(ddbPath, entry, id, &urls);
    }

    json generateStacData(const std::string& ddbPath, const std::string& entry) {
        return generateStacDocument(ddbPath, entry, "", nullptr);
    }

    // The entries an ItemCollection query selects: a WHERE clause over the entries, and the
    // instants (epoch seconds) it binds, in order
    struct ItemSelection {
        std::string where;
        std::optional<long long> start;
        std::optional<long long> end;

        void bind(Statement& q, int& idx) const {
            if (start)
                q.bind(idx++, *start);
            if (end)
                q.bind(idx++, *end);
        }
    };

    static ItemSelection itemSelection(const StacItemQuery& query) {
        // High-precision double formatter for inlining bbox literals (bind() has no double overload)
        const auto fmtD = [](double v)
        {
            std::ostringstream o;
            o << std::setprecision(15) << v;
            return o.str();
        };

        // Shared WHERE clause for the data and count queries
        ItemSelection selection;
        selection.where = "(polygon_geom IS NOT NULL OR point_geom IS NOT NULL)";

        const std::vector<double>& bbox = query.bbox;
        if (bbox.size() == 4)
        {
            // Geometries are stored in EPSG:4326; intersect against the request MBR
            selection.where += " AND MbrIntersects(BuildMbr(" + fmtD(bbox[0]) + ", " + fmtD(bbox[1]) +
                               ", " + fmtD(bbox[2]) + ", " + fmtD(bbox[3]) + ", 4326), "
                               "CASE WHEN polygon_geom IS NOT NULL THEN polygon_geom ELSE point_geom END)";
        }

        time_t dtStart = 0, dtEnd = 0;
        if (parseIso8601ToEpochSecs(query.datetimeStart, dtStart))
            selection.start = static_cast<long long>(dtStart);
        if (parseIso8601ToEpochSecs(query.datetimeEnd, dtEnd))
            selection.end = static_cast<long long>(dtEnd);

        // Per-entry instant (seconds): captureTime (ms) if present and > 0, else mtime
        const std::string instantExpr =
            "(CASE WHEN json_extract(properties, '$.captureTime') IS NOT NULL "
            "AND json_extract(properties, '$.captureTime') > 0 "
            "THEN json_extract(properties, '$.captureTime') / 1000.0 ELSE mtime END)";

        if (selection.start)
            selection.where += " AND " + instantExpr + " >= ?";
        if (selection.end)
            selection.where += " AND " + instantExpr + " <= ?";
        return selection;
    }

    // numberMatched: the entries matching the filters, ignoring paging
    static long long countSelected(Database* db, const ItemSelection& selection) {
        const auto cq = db->query("SELECT COUNT(*) FROM entries WHERE " + selection.where);
        int idx = 1;
        selection.bind(*cq, idx);
        return cq->fetch() ? cq->getInt64(0) : 0;
    }

    // The links of an ItemCollection
    static json itemCollectionLinks(const StacUrls& urls) {
        json links = json::array();
        if (!urls.catalogRoot.empty())
            links.push_back(stacLink("root", urls.catalogRoot + STAC_ENDPOINT, "application/json"));
        if (urls.collectionRoot != ".") {
            links.push_back(stacLink("self", urls.collectionRoot + STAC_ENDPOINT + "/items", "application/geo+json"));
            links.push_back(stacLink("collection", urls.collectionRoot + STAC_ENDPOINT, "application/json"));
        }
        return links;
    }

    // The STAC ItemCollection of the dataset; urls: null for the data-only one
    static json generateItemCollection(const std::string& ddbPath,
                                       const StacUrls* urls,
                                       const std::string& id,
                                       const StacItemQuery& query) {
        if (ddbPath.empty())
            throw AppException("No ddbPath is set for generating STAC");

        const int limit = query.limit <= 0 ? 10 : query.limit;
        const int offset = query.offset < 0 ? 0 : query.offset;

        auto db = open(ddbPath, false);
        const auto rootId = datasetRootId(db.get(), id);
        const ItemSelection selection = itemSelection(query);

        const std::string sql = std::string("SELECT ") + stacRowColumns + " FROM entries WHERE " +
                                selection.where + " ORDER BY path LIMIT ? OFFSET ?";
        const auto q = db->query(sql);
        int idx = 1;
        selection.bind(*q, idx);
        q->bind(idx++, static_cast<long long>(limit));
        q->bind(idx++, static_cast<long long>(offset));

        json features = json::array();
        while (q->fetch())
        {
            features.push_back(buildStacItem(readStacRow(*q), urls, rootId));
        }

        json fc;
        fc["type"] = "FeatureCollection";
        fc["features"] = features;
        fc["numberReturned"] = features.size();
        fc["numberMatched"] = countSelected(db.get(), selection);
        if (urls != nullptr)
            fc["links"] = itemCollectionLinks(*urls);
        return fc;
    }

    json generateStacItemCollection(const std::string& ddbPath,
                                    const std::string& stacCollectionRoot,
                                    const std::string& id,
                                    const std::string& stacCatalogRoot,
                                    const StacItemQuery& query) {
        const StacUrls urls{stacCollectionRoot, stacCatalogRoot};
        return generateItemCollection(ddbPath, &urls, id, query);
    }

    json generateStacItemCollectionData(const std::string& ddbPath, const StacItemQuery& query) {
        return generateItemCollection(ddbPath, nullptr, "", query);
    }

    // A STAC API bbox "minX,minY,maxX,maxY" (EPSG:4326): empty when absent; throws on an invalid one
    static std::vector<double> parseBbox(const std::string& bbox) {
        std::vector<double> values;
        if (str::trimView(bbox).empty())
            return values;

        std::stringstream ss(bbox);
        std::string token;
        while (std::getline(ss, token, ',')) {
            try {
                values.push_back(std::stod(token));
            } catch (...) {
                throw InvalidArgsException("Invalid bbox value: " + token);
            }
        }
        if (values.size() != 4)
            throw InvalidArgsException("bbox must contain exactly 4 comma-separated values");
        return values;
    }

    StacItemQuery parseStacItemQuery(const std::string& bbox, const std::string& datetime, int limit, int offset) {
        StacItemQuery query;
        query.bbox = parseBbox(bbox);

        // A single instant, or an interval "start/end" ("../end" or "start/.." for open ends);
        // an empty bound is open
        if (!str::trimView(datetime).empty()) {
            const auto slash = datetime.find('/');
            if (slash == std::string::npos) {
                query.datetimeStart = datetime;
                query.datetimeEnd = datetime;
            } else {
                const std::string lo = datetime.substr(0, slash);
                const std::string hi = datetime.substr(slash + 1);
                if (lo != "..")
                    query.datetimeStart = lo;
                if (hi != "..")
                    query.datetimeEnd = hi;
            }
        }
        query.limit = limit;
        query.offset = offset;
        return query;
    }
}
