/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef EPTTILER_H
#define EPTTILER_H

#include <gdal_priv.h>
#include <gdalwarper.h>
#include <ogr_srs_api.h>

#include <sstream>
#include <string>

#include "ddb_export.h"
#include "fs.h"
#include "geo.h"
#include "tiler.h"
#include "pointcloud.h"

namespace ddb {

class EptTiler {
    std::string eptPath;
    fs::path outputFolder;
    int tileSize;
    bool tms;

//    GDALDriverH pngDrv;
//    GDALDriverH memDrv;

    PointCloudInfo eptInfo;

    double oMinX, oMaxX, oMaxY, oMinY;
    GlobalMercator mercator;
    int tMaxZ;
    int tMinZ;

    GDALDatasetH createWarpedVRT(
        const GDALDatasetH &src, const OGRSpatialReferenceH &srs,
        GDALResampleAlg resampling = GRA_NearestNeighbour);

    // Returns parameters reading raster data.
    // (coordinates and x/y shifts for border tiles).
    // If the querysize is not given, the
    // extent is returned in the native resolution of dataset ds.
    GQResult geoQuery(GDALDatasetH ds, double ulx, double uly, double lrx,
                      double lry, int querySize = 0);

    // Convert from TMS to XYZ
    int tmsToXYZ(int ty, int tz) const;

    // Convert from XYZ to TMS
    int xyzToTMS(int ty, int tz) const;

   public:
    DDB_DLL EptTiler(const std::string &eptPath,
                  const std::string &outputFolder, int tileSize = 256,
                  bool tms = false);
    DDB_DLL ~EptTiler();

    DDB_DLL std::string getTilePath(int z, int x, int y,
                                    bool createIfNotExists);

    DDB_DLL std::string tile(int tz, int tx, int ty);
    DDB_DLL std::string tile(const TileInfo &tile);

    DDB_DLL std::vector<TileInfo> getTilesForZoomLevel(int tz) const;
    DDB_DLL BoundingBox<int> getMinMaxZ() const;

    // Min max tile coordinates for specified zoom level
    DDB_DLL BoundingBox<Projected2Di> getMinMaxCoordsForZ(int tz) const;
};

}  // namespace ddb

#endif  // EPTTILER_H
