/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef ENTRY_H
#define ENTRY_H

#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <ogr_srs_api.h>
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


    DDB_DLL void loadPointGeom(BasicPointGeometry *point_geom, const std::string& text);
    DDB_DLL void loadPolygonGeom(BasicPolygonGeometry *polygon_geom, const std::string& text);

struct Entry {
    std::string path = "";
    std::string hash = "";
    EntryType type = EntryType::Undefined;
    json meta;
    time_t mtime = 0;
    std::uintmax_t size = 0;
    int depth = 0;

    BasicPointGeometry point_geom;
    BasicPolygonGeometry polygon_geom;

    DDB_DLL void toJSON(json &j) const;
    DDB_DLL bool toGeoJSON(json &j, BasicGeometryType type = BasicGeometryType::BGAuto);
    DDB_DLL std::string toString();

    Entry() { }

    
    Entry(Statement& s) {

        LOGD << "Columns: " << s.getColumnsCount();

        // Expects a SELECT clause with: (order matters)
        // path, hash, type, meta, mtime, size, depth, AsGeoJSON(point_geom), AsGeoJSON(polygon_geom)
        this->path = s.getText(0);
        this->hash = s.getText(1);
        this->type = (EntryType)s.getInt(2);
        this->meta = json::parse(s.getText(3), nullptr, false);
        this->mtime = (time_t)s.getInt(4);
        this->size = s.getInt64(5);
        this->depth = s.getInt(6);
        

        if (s.getColumnsCount() == 9) {

            const auto point_geom_raw = s.getText(7);

            //LOGD << "point_geom: " << point_geom_raw;
            if (!point_geom_raw.empty()) ddb::loadPointGeom(&this->point_geom, point_geom_raw);
            //LOGD << "OK";

            const auto polygon_geom_raw = s.getText(8);

            //LOGD << "polygon_geom: " << polygon_geom_raw;
            if (!polygon_geom_raw.empty()) ddb::loadPolygonGeom(&this->polygon_geom, polygon_geom_raw);
            //LOGD << "OK";

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
