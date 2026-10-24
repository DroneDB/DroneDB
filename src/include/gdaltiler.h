/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef GDALTILER_H
#define GDALTILER_H

#include "gdal_inc.h"

#include <sstream>
#include <string>

#include "raster_stats.h"
#include "tiler.h"
#include "thumbs.h"

namespace ddb
{

    struct GeoExtent
    {
        int x;
        int y;
        int xsize;
        int ysize;
    };

    struct GQResult
    {
        GeoExtent r;
        GeoExtent w;
    };

    /// Renders web-map tiles from a georeferenced raster. The constructor opens the
    /// raster and prepares it (SRS, warped VRT to EPSG:3857, bands, zoom range); every
    /// tile then reads a window of it. An instance must not be used by two threads at
    /// once: RasterCache hands out one per caller and keeps them for the next tiles.
    class GDALTiler : public Tiler
    {
        // The raster file, then the VRT warped to EPSG:3857 over it when the file has another
        // SRS (declared after the file: closed before it)
        GDALDatasetUniquePtr fileDataset;
        GDALDatasetUniquePtr warpedDataset;
        // What the tiles read: the warped VRT, or the file (not owned)
        GDALDatasetH inputDataset = nullptr;

        int rasterCount;

        bool hasGeoreference(const GDALDatasetH &dataset);
        bool sameProjection(const OGRSpatialReferenceH &a,
                            const OGRSpatialReferenceH &b);
        int dataBandsCount(const GDALDatasetH &dataset);
        GDALDatasetUniquePtr createWarpedVRT(
            const GDALDatasetH &src, const OGRSpatialReferenceH &srs,
            GDALResampleAlg resampling = GRA_NearestNeighbour);
        void open();

        // Returns parameters reading raster data.
        // (coordinates and x/y shifts for border tiles).
        // If the querysize is not given, the
        // extent is returned in the native resolution of dataset ds.
        GQResult geoQuery(GDALDatasetH ds, double ulx, double uly, double lrx,
                          double lry, int querySize = 0);

        // Window of the raster covered by the tile (XYZ row); false when it is empty
        bool tileWindow(int tz, int tx, int ty, GQResult &g);

        // Reads the first bands (stretched to Byte when needed) and the alpha of window g
        void readBands(const GQResult &g, int bands, RasterImage &image);
        void readAlpha(const GQResult &g, RasterImage &image);
        void readSelectedBands(const GQResult &g, const std::vector<int> &bands, RasterImage &image);
        void renderFormula(const GQResult &g, const ThumbVisParams &visParams, RasterImage &image);

        // Statistics of a band of the raster file (not of the warped VRT)
        BandStatistics bandStatistics(int band);

        GDALRasterBandH FindAlphaBand(const GDALDatasetH &dataset);

    public:
        GDALTiler(const std::string &geotiffPath,
                  const std::string &outputFolder, int tileSize = 256,
                  bool tms = false);
        ~GDALTiler();

        using Tiler::tile;

        std::string tile(int tz, int tx, int ty, ImageFormat format,
                         uint8_t **outBuffer = nullptr, int *outBufferSize = nullptr) override;

        // Format-aware overload. outputFormat is "png", "jpeg" or "webp" (or a MIME type).
        std::string tile(int tz, int tx, int ty,
                         const std::string &outputFormat,
                         uint8_t **outBuffer = nullptr, int *outBufferSize = nullptr);

        // Band-aware tile generation with visualization parameters
        std::string tile(int tz, int tx, int ty,
                         const ThumbVisParams &visParams,
                         uint8_t **outBuffer = nullptr, int *outBufferSize = nullptr);
        std::string tile(int tz, int tx, int ty,
                         const ThumbVisParams &visParams, ImageFormat format,
                         uint8_t **outBuffer = nullptr, int *outBufferSize = nullptr);
    };

} // namespace ddb

#endif // GDALTILER_H
