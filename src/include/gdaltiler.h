/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef GDALTILER_H
#define GDALTILER_H

#include "gdal_inc.h"

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "raster_stats.h"
#include "tiler.h"
#include "thumbs.h"

namespace ddb
{
    struct BandStylePlan; // vis_render.h

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

    /// Renders web-map tiles from a georeferenced raster that libddb wrote (an artefact,
    /// raster_artefacts.h: a COG, a geoprojected photo; TilerHelper makes one from a file of a
    /// user). The constructor opens the raster and prepares it (SRS, warped VRT to EPSG:3857, bands, zoom range); every
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

        // The NoData value that replaces the one of the bands during a tile with one (FR-RAS-31): the
        // tile reads a view of the file that has it (inputDataset), and its statistics are those of
        // the pixels that view leaves
        std::optional<double> nodataOverride_;

        // Puts a view of inputDataset with the NoData override in its place while it lives (an
        // instance is used by one thread at a time); nothing when there is no override
        class NodataOverrideScope
        {
            GDALTiler &tiler_;
            GDALDatasetUniquePtr view_;
            GDALDatasetH previous_;

        public:
            NodataOverrideScope(GDALTiler &tiler, const std::optional<double> &nodata);
            ~NodataOverrideScope();
            NodataOverrideScope(const NodataOverrideScope &) = delete;
            NodataOverrideScope &operator=(const NodataOverrideScope &) = delete;
        };
        friend class NodataOverrideScope;

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
        // Places the RGBA (interleaved) of window g in the tile image: three bands and its alpha
        static void placeRgba(const std::vector<uint8_t> &rgba, const GQResult &g, RasterImage &image);
        // The tile of one band painted with a colormap (FR-RAS-01), transparent where the raster has no data or
        // is not (out of its bounds too, as the plain tile is)
        std::string styledTile(int tz, int tx, int ty, const BandStylePlan &style, const std::optional<double> &nodata,
                               ImageFormat format, uint8_t **outBuffer, int *outBufferSize);

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
