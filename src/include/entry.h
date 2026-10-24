/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef ENTRY_H
#define ENTRY_H

#include <optional>

#include "gdal_inc.h"
#include "entry_types.h"
#include "logger.h"
#include "exceptions.h"
#include "hash.h"
#include "exif.h"
#include "basicgeometry.h"
#include "geo.h"
#include "json.h"
#include "fs.h"

namespace ddb
{

    struct Entry
    {
        std::string path = "";
        std::string hash = "";
        EntryType type = EntryType::Undefined;
        json properties;
        time_t mtime = 0;
        std::uintmax_t size = 0;
        int depth = 0;

        BasicPointGeometry point_geom;
        BasicPolygonGeometry polygon_geom;

        json meta;

        /// The entry as a GeoJSON feature of its first geometry of `type` (the other kinds of
        /// geometry are left out: QGIS does not read GeometryCollections), with the entry as its
        /// properties; nothing when the entry has no such geometry
        std::optional<json> toGeoJSON(BasicGeometryType type = BasicGeometryType::BGAuto) const;
        std::string toString() const;

        Entry() {}

        // The columns of an index row as the database stores them (the JSON ones as text)
        struct Row
        {
            std::string path{};
            std::string hash{};
            int type = 0;
            std::string propertiesJson{};
            long long mtime = 0;
            std::uintmax_t size = 0;
            int depth = 0;
            std::string pointCoordinatesJson{}; // "" when the row has none
            std::string polyCoordinatesJson{};
            std::string metaJson{};
        };

        void parseFields(const Row &row)
        {
            this->path = row.path;
            this->hash = row.hash;
            this->type = static_cast<EntryType>(row.type);
            try
            {
                this->properties = json::parse(row.propertiesJson, nullptr, false);
            }
            catch (json::exception &e)
            {
                LOGD << "Invalid entry JSON: " << e.what();
            }
            this->mtime = static_cast<time_t>(row.mtime);
            this->size = row.size;
            this->depth = row.depth;

            this->parsePointGeometry(row.pointCoordinatesJson);
            this->parsePolygonGeometry(row.polyCoordinatesJson);
            this->parseMeta(row.metaJson);
        }

        void parsePointGeometry(const std::string &coordinatesJson)
        {
            point_geom.clear();
            if (coordinatesJson.empty())
                return;

            json j;
            try
            {
                j = json::parse(coordinatesJson);
            }
            catch (json::exception &e)
            {
                throw JSONException(e.what());
            }

            if (!j.is_array() || j.size() < 2)
            {
                LOGD << "Invalid coordinatesJson: " << coordinatesJson;
                return;
            }

            if (j.size() == 2)
            {
                point_geom.addPoint(j[0].get<double>(), j[1].get<double>(), 0);
            }
            else if (j.size() >= 3)
            {
                point_geom.addPoint(j[0].get<double>(), j[1].get<double>(), j[2].get<double>());
            }
        }

        void parsePolygonGeometry(const std::string &coordinatesJson)
        {
            polygon_geom.clear();
            if (coordinatesJson.empty())
                return;

            json j;
            try
            {
                j = json::parse(coordinatesJson);
            }
            catch (json::exception &e)
            {
                throw JSONException(e.what());
            }

            if (!j.is_array() || j.size() != 1)
            {
                LOGD << "Invalid coordinatesJson: " << coordinatesJson;
                return;
            }

            json ring = j[0];
            for (auto &coord : ring)
            {
                if (coord.size() == 2)
                {
                    polygon_geom.addPoint(coord[0].get<double>(), coord[1].get<double>(), 0);
                }
                else if (coord.size() >= 3)
                {
                    polygon_geom.addPoint(coord[0].get<double>(), coord[1].get<double>(), coord[2].get<double>());
                }
            }
        }

        void parseMeta(const std::string &metaJson)
        {
            meta.clear();
            if (metaJson.empty())
                return;

            try
            {
                meta = json::parse(metaJson);
            }
            catch (json::exception &)
            {
                LOGD << "Corrupted meta: " << metaJson;
                return;
            }
        }
    };

    /// The JSON of an entry (ADL: `json(entry)`, `j.get<Entry>()`). The shape is frozen by the
    /// contract fixtures: path, hash (when set), type, properties (when not empty), mtime,
    /// size, depth, point_geom and polygon_geom (when set) and meta (when not empty).
    /// from_json reads path, hash, type, size, depth and mtime.
    void to_json(json &j, const Entry &e);
    void from_json(const json &j, Entry &e);

    /** Parse an entry
     * @param path path to file
     * @param rootDirectory root directory from which to compute relative path
     * @param entry reference to output Entry object
     * @param withHash whether to compute the hash of the file (slow)
     */
    void parseEntry(const fs::path &path, const fs::path &rootDirectory, Entry &entry, bool withHash = true);

    /** Parse an entry with context reuse
     * @param path path to file
     * @param rootDirectory root directory from which to compute relative path
     * @param entry reference to output Entry object
     * @param withHash whether to compute the hash of the file (slow)
     * @param ctx reference to FingerprintContext for caching Exiv2 data between fingerprint and parseEntry
     */
    void parseEntry(const fs::path &path, const fs::path &rootDirectory, Entry &entry, bool withHash, FingerprintContext &ctx);

    Geographic2D getRasterCoordinate(OGRCoordinateTransformationH hTransform, const double *geotransform, double x, double y);
    void calculateFootprint(const SensorSize &sensorSize, const GeoLocation &geo, const Focal &focal, const CameraOrientation &cameraOri, double relAltitude, BasicGeometry &geom);
    void parseDroneDBEntry(const fs::path &ddbPath, Entry &entry);

    /** Identify whether a file is an Image, GeoImage, Georaster or something else
     * as quickly as possible. Does not fingerprint for other types. */
    EntryType fingerprint(const fs::path &path);

    /** Identify whether a file is an Image, GeoImage, Georaster or something else
     * and populate the FingerprintContext with Exiv2 data for reuse.
     * @param path path to file
     * @param ctx reference to FingerprintContext that will be populated with Exiv2 data
     */
    EntryType fingerprint(const fs::path &path, FingerprintContext &ctx);

}

#endif // ENTRY_H
