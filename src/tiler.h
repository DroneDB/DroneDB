/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TILER_H
#define TILER_H

#include <gdal_priv.h>
#include <gdalwarper.h>
#include <ogr_srs_api.h>

#include <sstream>
#include <string>

#include "ddb_export.h"
#include "fs.h"
#include "geo.h"

namespace ddb {

class GlobalMercator {
    int tileSize;
    double originShift;
    double initialResolution;
    int maxZoomLevel;

   public:
    GlobalMercator(int tileSize);

    BoundingBox<Geographic2D> tileLatLonBounds(int tx, int ty, int zoom) const;

    // Bounds of the given tile in EPSG:3857 coordinates
    BoundingBox<Projected2D> tileBounds(int tx, int ty, int zoom) const;

    // Converts XY point from Spherical Mercator EPSG:3857 to lat/lon in WGS84
    // Datum
    Geographic2D metersToLatLon(double mx, double my) const;

    // Tile for given mercator coordinates
    Projected2Di metersToTile(double mx, double my, int zoom) const;

    // Converts pixel coordinates in given zoom level of pyramid to EPSG:3857"
    Projected2D pixelsToMeters(int px, int py, int zoom) const;

    // Converts EPSG:3857 to pyramid pixel coordinates in given zoom level
    Projected2D metersToPixels(double mx, double my, int zoom) const;

    // Tile covering region in given pixel coordinates
    Projected2Di pixelsToTile(double px, double py) const;

    // Resolution (meters/pixel) for given zoom level (measured at Equator)
    double resolution(int zoom) const;

    // Minimum zoom level that can fully contains a line of meterLength
    int zoomForLength(double meterLength) const;

    // Maximal scaledown zoom of the pyramid closest to the pixelSize.
    int zoomForPixelSize(double pixelSize) const;
};

struct TileInfo {
    int tx, ty, tz;
    TileInfo(int tx, int ty, int tz) : tx(tx), ty(ty), tz(tz){};
};

class Tiler {
protected:
    std::string inputPath;
    fs::path outputFolder;
    int tileSize;
    bool tms;
    int nBands;
    double oMinX, oMaxX, oMaxY, oMinY;
    GlobalMercator mercator;
    int tMaxZ;
    int tMinZ;

    // Convert from TMS to XYZ
    int tmsToXYZ(int ty, int tz) const;

    // Convert from XYZ to TMS
    int xyzToTMS(int ty, int tz) const;

    template <typename T>
    void rescale(GDALRasterBandH hBand, char *buffer, size_t bufsize);

   public:
    DDB_DLL Tiler(const std::string &inputPath,
                  const std::string &outputFolder, int tileSize = 256,
                  bool tms = false);
    DDB_DLL ~Tiler();

    DDB_DLL std::string getTilePath(int z, int x, int y,
                                    bool createIfNotExists);

    DDB_DLL virtual std::string tile(int tz, int tx, int ty) = 0;
    DDB_DLL std::string tile(const TileInfo &tile);

    DDB_DLL std::vector<TileInfo> getTilesForZoomLevel(int tz) const;
    DDB_DLL BoundingBox<int> getMinMaxZ() const;

    // Min max tile coordinates for specified zoom level
    DDB_DLL BoundingBox<Projected2Di> getMinMaxCoordsForZ(int tz) const;
};


}  // namespace ddb

#endif  // TILER_H
