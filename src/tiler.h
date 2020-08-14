/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TILER_H
#define TILER_H

#include <gdal_priv.h>
#include <ogr_srs_api.h>
#include <gdalwarper.h>
#include <random>
#include <sstream>
#include <string>
#include "geo.h"
#include "fs.h"

namespace ddb{

struct GeoExtent{
    int x;
    int y;
    int xsize;
    int ysize;
};

struct GQResult{
    GeoExtent r;
    GeoExtent w;
};

class Tiler
{
    int tileSize;
    std::string geotiffPath;
    fs::path outputFolder;

    bool hasGeoreference(const GDALDatasetH &dataset);
    bool sameProjection(const OGRSpatialReferenceH &a, const OGRSpatialReferenceH &b);

    static std::random_device              rd;
    static std::mt19937                    gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    static std::uniform_int_distribution<> dis2(8, 11);
    std::string uuidv4();

    int dataBandsCount(const GDALDatasetH &dataset);
    std::string getTilePath(int z, int x, int y, bool createIfNotExists);
    GDALDatasetH createWarpedVRT(const GDALDatasetH &src, const OGRSpatialReferenceH &srs, GDALResampleAlg resampling =  GRA_NearestNeighbour);

    // Returns parameters reading raster data.
    // (coordinates and x/y shifts for border tiles).
    // If the querysize is not given, the
    // extent is returned in the native resolution of dataset ds.
    GQResult geoQuery()
public:
    Tiler(const std::string &geotiffPath, const std::string &outputFolder);

    std::string tile(int tz, int tx, int ty);

};



class GlobalMercator{
    double originShift;
    double initialResolution;
    int maxZoomLevel;
    int tileSize;
public:
    GlobalMercator();

    BoundingBox<Geographic2D> tileLatLonBounds(int tx, int ty, int zoom);

    // Bounds of the given tile in EPSG:3857 coordinates
    BoundingBox<Projected2D> tileBounds(int tx, int ty, int zoom);

    // Converts XY point from Spherical Mercator EPSG:3857 to lat/lon in WGS84 Datum
    Geographic2D metersToLatLon(int mx, int my);

    // Tile for given mercator coordinates
    Projected2D metersToTile(int mx, int my, int zoom);

    // Converts pixel coordinates in given zoom level of pyramid to EPSG:3857"
    Projected2D pixelsToMeters(int px, int py, int zoom);

    // Converts EPSG:3857 to pyramid pixel coordinates in given zoom level
    Projected2D metersToPixels(int mx, int my, int zoom);

    // Tile covering region in given pixel coordinates
    Projected2D pixelsToTile(int px, int py);

    // Resolution (meters/pixel) for given zoom level (measured at Equator)
    double resolution(int zoom);

    // Maximal scaledown zoom of the pyramid closest to the pixelSize.
    int zoomForPixelSize(double pixelSize);


};

}

#endif // TILER_H
