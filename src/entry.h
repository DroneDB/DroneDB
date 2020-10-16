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

        this->path = s.getText(0);
        this->hash = s.getText(1);
        this->type = (EntryType)s.getInt(2);
        this->meta = json::parse(s.getText(3), nullptr, false);
        this->mtime = (time_t)s.getInt(4);
        this->size = s.getInt64(5);
        this->depth = s.getInt(6);
        
        // TODO by HeDo: I don't know how to parse these. They are blobs, cannot use getText
        // this->point_geom = ?
        // this->polygon_geom = ?
    }
};

DDB_DLL bool parseEntry(const fs::path &path, const fs::path &rootDirectory, Entry &entry, bool wishHash = true, bool stopOnError = true);
DDB_DLL Geographic2D getRasterCoordinate(OGRCoordinateTransformationH hTransform, double *geotransform, double x, double y);
DDB_DLL void calculateFootprint(const SensorSize &sensorSize, const GeoLocation &geo, const Focal &focal, const CameraOrientation &cameraOri, double relAltitude, BasicGeometry &geom);

}

#endif // ENTRY_H
