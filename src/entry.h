/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef ENTRY_H
#define ENTRY_H

#include "gdal_inc.h"
#include "entry_types.h"
#include "logger.h"
#include "exceptions.h"
#include "utils.h"
#include "hash.h"
#include "exif.h"
#include "basicgeometry.h"
#include "geo.h"
#include "json.h"
#include "fs.h"
#include "ddb_export.h"

namespace ddb {

struct Entry {
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

    DDB_DLL void toJSON(json &j) const;
    DDB_DLL std::string toJSONString() const;
    DDB_DLL void fromJSON(const json &j);
    DDB_DLL bool toGeoJSON(json &j, BasicGeometryType type = BasicGeometryType::BGAuto);
    DDB_DLL std::string toString();

    Entry() { }

    Entry(const std::string &path, const std::string &hash,
          int type, const std::string &propertiesJson,
          long long mtime, std::uintmax_t size, int depth,
          const std::string &pointCoordinatesJson = "", const std::string &polyCoordinatesJson = "", const std::string &metaJson = ""){
        parseFields(path, hash, type, propertiesJson, mtime, size, depth, pointCoordinatesJson, polyCoordinatesJson, metaJson);
    }

    void parseFields(const std::string &path, const std::string &hash,
                     int type, const std::string &propertiesJson,
                     long long mtime, std::uintmax_t size, int depth,
                     const std::string &pointCoordinatesJson = "", const std::string &polyCoordinatesJson = "", const std::string &metaJson = ""){
        this->path = path;
        this->hash = hash;
        this->type = static_cast<EntryType>(type);
        try{
            this->properties = json::parse(propertiesJson, nullptr, false);
        }catch(json::exception &e){
            LOGD << "Invalid entry JSON: " << e.what();
        }
        this->mtime = static_cast<time_t>(mtime);
        this->size = size;
        this->depth = depth;

        this->parsePointGeometry(pointCoordinatesJson);
        this->parsePolygonGeometry(polyCoordinatesJson);
        this->parseMeta(metaJson);
    }

    void parsePointGeometry(const std::string &coordinatesJson){
        point_geom.clear();
        if (coordinatesJson.empty()) return;

        json j;
        try{
            j = json::parse(coordinatesJson);
        }catch(json::exception &e){
            throw JSONException(e.what());
        }

        if (!j.is_array() || j.size() < 2){
            LOGD << "Invalid coordinatesJson: " << coordinatesJson;
            return;
        }

        if (j.size() == 2){
            point_geom.addPoint(j[0].get<double>(), j[1].get<double>(), 0);
        }else if (j.size() >= 3){
            point_geom.addPoint(j[0].get<double>(), j[1].get<double>(), j[2].get<double>());
        }
    }

    void parsePolygonGeometry(const std::string &coordinatesJson){
        polygon_geom.clear();
        if (coordinatesJson.empty()) return;

        json j;
        try{
            j = json::parse(coordinatesJson);
        }catch(json::exception &e){
            throw JSONException(e.what());
        }

        if (!j.is_array() || j.size() != 1){
            LOGD << "Invalid coordinatesJson: " << coordinatesJson;
            return;
        }

        json ring = j[0];
        for (auto &coord : ring){
            if (coord.size() == 2){
                polygon_geom.addPoint(coord[0].get<double>(), coord[1].get<double>(), 0);
            }else if (coord.size() >= 3){
                polygon_geom.addPoint(coord[0].get<double>(), coord[1].get<double>(), coord[2].get<double>());
            }
        }
    }

    void parseMeta(const std::string &metaJson){
        meta.clear();
        if (metaJson.empty()) return;

        try{
            meta = json::parse(metaJson);
        }catch(json::exception &){
            LOGD << "Corrupted meta: " << metaJson;
            return;
        }
    }
};

/** Parse an entry
 * @param path path to file
 * @param rootDirectory root directory from which to compute relative path
 * @param entry reference to output Entry object
 * @param withHash whether to compute the hash of the file (slow)
 */
DDB_DLL void parseEntry(const fs::path &path, const fs::path &rootDirectory, Entry &entry, bool wishHash = true);
DDB_DLL Geographic2D getRasterCoordinate(OGRCoordinateTransformationH hTransform, double *geotransform, double x, double y);
DDB_DLL void calculateFootprint(const SensorSize &sensorSize, const GeoLocation &geo, const Focal &focal, const CameraOrientation &cameraOri, double relAltitude, BasicGeometry &geom);
DDB_DLL void parseDroneDBEntry(const fs::path &ddbPath, Entry &entry);

/** Identify whether a file is an Image, GeoImage, Georaster or something else
 * as quickly as possible. Does not fingerprint for other types. */
DDB_DLL EntryType fingerprint(const fs::path &path);

}

#endif // ENTRY_H
