/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TILER_H
#define TILER_H

#include "gdal_inc.h"

#include <sstream>
#include <string>

#include "fs.h"
#include "geo.h"
#include "tile_encoder.h"

namespace ddb
{

    class GlobalMercator
    {
        int tileSize;
        double originShift;
        double initialResolution;
        int maxZoomLevel;

    public:
        GlobalMercator(int tileSize);

        // Bounds of the given tile in EPSG:3857 coordinates
        BoundingBox<Projected2D> tileBounds(int tx, int ty, int zoom) const;

        // Tile for given mercator coordinates
        Projected2Di metersToTile(double mx, double my, int zoom) const;

        // Converts pixel coordinates in given zoom level of pyramid to EPSG:3857"
        Projected2D pixelsToMeters(double px, double py, int zoom) const;

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

    struct TileInfo
    {
        int tx, ty, tz;
        TileInfo(int tx, int ty, int tz) : tx(tx), ty(ty), tz(tz) {};
    };

    class Tiler
    {
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

        // Encodes image as tile (tz, tx, ty) of this tiler: into *outBuffer when set,
        // otherwise into the tile file under the output folder (or /vsimem/), whose path
        // is returned
        std::string writeTile(const RasterImage &image, ImageFormat format, int tz, int tx, int ty,
                              uint8_t **outBuffer, int *outBufferSize);

    public:
        Tiler(const std::string &inputPath,
              const std::string &outputFolder, int tileSize = 256,
              bool tms = false);
        virtual ~Tiler();

        std::string getTilePath(int z, int x, int y,
                                bool createIfNotExists);

        // PNG tile
        std::string tile(int tz, int tx, int ty, uint8_t **outBuffer = nullptr, int *outBufferSize = nullptr);
        virtual std::string tile(int tz, int tx, int ty, ImageFormat format,
                                 uint8_t **outBuffer = nullptr, int *outBufferSize = nullptr) = 0;
        std::string tile(const TileInfo &tile);

        std::vector<TileInfo> getTilesForZoomLevel(int tz) const;
        BoundingBox<int> getMinMaxZ() const;

        // Min max tile coordinates for specified zoom level
        BoundingBox<Projected2Di> getMinMaxCoordsForZ(int tz) const;
    };

} // namespace ddb

#endif // TILER_H
