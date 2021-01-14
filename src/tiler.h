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

    // Maximal scaledown zoom of the pyramid closest to the pixelSize.
    int zoomForPixelSize(double pixelSize) const;
};

struct GeoExtent {
    int x;
    int y;
    int xsize;
    int ysize;
};

struct GQResult {
    GeoExtent r;
    GeoExtent w;
};

struct TileInfo {
    int tx, ty, tz;
    TileInfo(int tx, int ty, int tz) : tx(tx), ty(ty), tz(tz){};
};

class Tiler {
    std::string geotiffPath;
    fs::path outputFolder;
    int tileSize;
    bool tms;

    GDALDriverH pngDrv;
    GDALDriverH memDrv;

    GDALDatasetH inputDataset = nullptr;
    GDALDatasetH origDataset = nullptr;
    
    int rasterCount;
    int nBands;

    double oMinX, oMaxX, oMaxY, oMinY;
    GlobalMercator mercator;
    int tMaxZ;
    int tMinZ;

    bool hasGeoreference(const GDALDatasetH &dataset);
    bool sameProjection(const OGRSpatialReferenceH &a,
                        const OGRSpatialReferenceH &b);

    int dataBandsCount(const GDALDatasetH &dataset);
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

    template <typename T>
    void rescale(GDALRasterBandH hBand, char *buffer, size_t bufsize);

   public:
    DDB_DLL Tiler(const std::string &geotiffPath,
                  const std::string &outputFolder, int tileSize = 256,
                  bool tms = false);
    DDB_DLL ~Tiler();

    DDB_DLL std::string getTilePath(int z, int x, int y,
                                    bool createIfNotExists);

    DDB_DLL std::string tile(int tz, int tx, int ty);
    DDB_DLL std::string tile(const TileInfo &tile);

    DDB_DLL std::vector<TileInfo> getTilesForZoomLevel(int tz) const;
    DDB_DLL BoundingBox<int> getMinMaxZ() const;

    // Min max tile coordinates for specified zoom level
    DDB_DLL BoundingBox<Projected2Di> getMinMaxCoordsForZ(int tz) const;
};

class TilerHelper {
    // Parses a string (either "N" or "min-max") and returns
    // two ints (min,max)
    static BoundingBox<int> parseZRange(const std::string &zRange);

    // Where to store local cache tiles
    static fs::path getCacheFolderName(const fs::path &tileablePath,
                                       time_t modifiedTime, int tileSize);

   public:
    DDB_DLL static void runTiler(Tiler &tiler, std::ostream &output = std::cout,
                                 const std::string &format = "text",
                                 const std::string &zRange = "auto",
                                 const std::string &x = "auto",
                                 const std::string &y = "auto");

    // Get a single tile from user cache
    DDB_DLL static fs::path getFromUserCache(const fs::path &tileablePath,
                                             int tz, int tx, int ty,
                                             int tileSize, bool tms,
                                             bool forceRecreate);

    // Prepare a tileable file for tiling (if needed)
    // for example, geoimages that can be tiled are first geoprojected
    DDB_DLL static fs::path toGeoTIFF(const fs::path &tileablePath,
                                      int tileSize, bool forceRecreate,
                                      const fs::path &outputGeotiff = "");

    DDB_DLL static void cleanupUserCache();
};

}  // namespace ddb

#endif  // TILER_H
