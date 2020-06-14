/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef ENTRY_H
#define ENTRY_H

#include <filesystem>
#include "gdal_priv.h"
#include "ogrsf_frmts.h"
#include "ogr_srs_api.h"
#include "json_fwd.hpp"
#include "json.hpp"
#include "types.h"
#include "../logger.h"
#include "../classes/exceptions.h"
#include "../utils.h"
#include "../classes/hash.h"
#include "../classes/exif.h"
#include "../classes/basicgeometry.h"
#include "../libs/geo.h"

namespace fs = std::filesystem;

namespace ddb {

using json = nlohmann::json;

struct Entry {
    std::string path = "";
    std::string hash = "";
    EntryType type = EntryType::Undefined;
    json meta;
    time_t mtime = 0;
    off_t size = 0;
    int depth = 0;

    BasicPointGeometry point_geom;
    BasicPolygonGeometry polygon_geom;

    void toJSON(json &j);
    bool toGeoJSON(json &j, BasicGeometryType type = BasicGeometryType::BGAuto);
    std::string toString();
};

struct ParseEntryOpts{
    bool withHash = true;
    bool stopOnError = true;
};

bool parseEntry(const fs::path &path, const fs::path &rootDirectory, Entry &entry, ParseEntryOpts &opts);
Geographic2D getRasterCoordinate(OGRCoordinateTransformationH hTransform, double *geotransform, double x, double y);
void calculateFootprint(const exif::SensorSize &sensorSize, const exif::GeoLocation &geo, const exif::Focal &focal, const exif::CameraOrientation &cameraOri, double relAltitude, BasicGeometry &geom);

}

#endif // ENTRY_H
