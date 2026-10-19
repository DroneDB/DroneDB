/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gdaltiler.h"

#include <mutex>
#include <memory>
#include <vector>
#include <algorithm>
#include <sstream>
#include <cmath>
#include <cstring>

#include "entry.h"
#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "vis_render.h"

namespace ddb
{

    bool GDALTiler::hasGeoreference(const GDALDatasetH &dataset)
    {
        double geo[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
        if (GDALGetGeoTransform(dataset, geo) != CE_None)
            throw GDALException("Cannot fetch geotransform in hasGeoreference");

        return (geo[0] != 0.0 || geo[1] != 1.0 || geo[2] != 0.0 || geo[3] != 0.0 ||
                geo[4] != 0.0 || geo[5] != 1.0) ||
               GDALGetGCPCount(dataset) != 0;
    }

    bool GDALTiler::sameProjection(const OGRSpatialReferenceH &a,
                                   const OGRSpatialReferenceH &b)
    {
        char *aProj = nullptr;
        char *bProj = nullptr;

        if (OSRExportToProj4(a, &aProj) != CE_None)
            throw GDALException("Cannot export proj4");
        if (OSRExportToProj4(b, &bProj) != CE_None)
        {
            CPLFree(aProj);
            throw GDALException("Cannot export proj4");
        }

        bool result = std::string(aProj) == std::string(bProj);

        CPLFree(aProj);
        CPLFree(bProj);

        return result;
    }

    int GDALTiler::dataBandsCount(const GDALDatasetH &dataset)
    {
        const GDALRasterBandH raster = GDALGetRasterBand(dataset, 1);
        const GDALRasterBandH alphaBand = GDALGetMaskBand(raster);
        const int bandsCount = GDALGetRasterCount(dataset);
        const GDALRasterBandH lastBand = GDALGetRasterBand(dataset, bandsCount);

        if (GDALGetMaskFlags(alphaBand) & GMF_ALPHA || bandsCount == 4 ||
            bandsCount == 2 || GDALGetRasterColorInterpretation(lastBand) == GCI_AlphaBand)
        {
            return bandsCount - 1;
        }

        return bandsCount;
    }
    namespace
    {
        struct SrsDeleter
        {
            void operator()(void *srs) const { OSRDestroySpatialReference(static_cast<OGRSpatialReferenceH>(srs)); }
        };
        using SrsPtr = std::unique_ptr<void, SrsDeleter>;

        // Maps the values of a window (band-sequential, windowPixels per band) to 0-255
        // over [bMin, bMax] and writes them at their place in the tile image
        template <typename T>
        void rescaleInto(const uint8_t *buffer, int bands, const GQResult &g, double bMin, double bMax,
                         RasterImage &image)
        {
            // Avoid divide by zero
            if (bMin == bMax)
                bMax += 0.1;

            LOGD << "Min: " << bMin << " | Max: " << bMax;

            // Can still happen according to GDAL for very large values
            if (bMin == bMax)
                throw GDALException("Cannot scale values due to source min/max being equal");

            const double deltamm = bMax - bMin;
            const T *src = reinterpret_cast<const T *>(buffer);
            const size_t windowPixels = static_cast<size_t>(g.w.xsize) * g.w.ysize;

            for (int b = 0; b < bands; b++)
            {
                uint8_t *dst = image.band(b);
                for (int row = 0; row < g.w.ysize; row++)
                {
                    const T *in = src + b * windowPixels + static_cast<size_t>(row) * g.w.xsize;
                    uint8_t *out = dst + static_cast<size_t>(g.w.y + row) * image.width + g.w.x;
                    for (int col = 0; col < g.w.xsize; col++)
                    {
                        const double v = std::max(bMin, std::min(bMax, static_cast<double>(in[col])));
                        out[col] = static_cast<uint8_t>(255.0 * (v - bMin) / deltamm);
                    }
                }
            }
        }

        // Copies one plane of a window into the tile image plane
        void placeWindow(const uint8_t *window, const GQResult &g, uint8_t *plane, int planeWidth)
        {
            for (int row = 0; row < g.w.ysize; row++)
                std::memcpy(plane + static_cast<size_t>(g.w.y + row) * planeWidth + g.w.x,
                            window + static_cast<size_t>(row) * g.w.xsize, g.w.xsize);
        }

    }

    GDALTiler::GDALTiler(const std::string &inputPath, const std::string &outputFolder,
                         int tileSize, bool tms)
        : Tiler(inputPath, outputFolder, tileSize, tms)
    {
        try
        {
            open();
        }
        catch (...)
        {
            // The destructor does not run when the constructor throws
            if (inputDataset && inputDataset != origDataset)
                GDALClose(inputDataset);
            if (origDataset)
                GDALClose(origDataset);
            throw;
        }
    }

    void GDALTiler::open()
    {
        std::string openPath = inputPath;
        if (utils::isNetworkPath(openPath))
        {
            CPLSetConfigOption("GDAL_DISABLE_READDIR_ON_OPEN", "YES");
            CPLSetConfigOption("CPL_VSIL_CURL_ALLOWED_EXTENSIONS", ".tif,.tiff");

            openPath = "/vsicurl/" + openPath;
        }

        inputDataset = GDALOpen(openPath.c_str(), GA_ReadOnly);
        if (inputDataset == nullptr)
            throw GDALException("Cannot open " + openPath);

        rasterCount = GDALGetRasterCount(inputDataset);
        if (rasterCount == 0)
            throw GDALException("No raster bands found in " + openPath);

        // Extract input SRS
        const SrsPtr inputSrs(OSRNewSpatialReference(nullptr));
        std::string inputSrsWkt;
        if (GDALGetProjectionRef(inputDataset) != nullptr)
        {
            inputSrsWkt = GDALGetProjectionRef(inputDataset);
        }
        else if (GDALGetGCPCount(inputDataset) > 0)
        {
            inputSrsWkt = GDALGetGCPProjection(inputDataset);
        }
        else
        {
            throw GDALException("No projection found in " + openPath);
        }

        char *wktp = const_cast<char *>(inputSrsWkt.c_str());
        if (OSRImportFromWkt(inputSrs.get(), &wktp) != OGRERR_NONE)
            throw GDALException("Cannot read spatial reference system for " + openPath + ". Is PROJ available?");

        // Setup output SRS
        const SrsPtr outputSrs(OSRNewSpatialReference(nullptr));
        OSRImportFromEPSG(outputSrs.get(), 3857); // TODO: support for geodetic?

        if (!hasGeoreference(inputDataset))
            throw GDALException(openPath + " is not georeferenced.");

        // Check if we need to reproject
        if (!sameProjection(inputSrs.get(), outputSrs.get()))
        {
            origDataset = inputDataset;
            inputDataset = createWarpedVRT(inputDataset, outputSrs.get());
        }

        nBands = dataBandsCount(inputDataset);

        double outGt[6];
        if (GDALGetGeoTransform(inputDataset, outGt) != CE_None)
            throw GDALException("Cannot fetch geotransform outGt");

        // Validate geotransform values
        if (std::abs(outGt[1]) < std::numeric_limits<double>::epsilon() ||
            std::abs(outGt[5]) < std::numeric_limits<double>::epsilon())
        {
            throw GDALException("Invalid geotransform: pixel size is zero");
        }

        oMinX = outGt[0];
        oMaxX = outGt[0] + GDALGetRasterXSize(inputDataset) * outGt[1];
        oMaxY = outGt[3];
        oMinY = outGt[3] - GDALGetRasterYSize(inputDataset) * outGt[1];

        LOGD << "Bounds (output SRS): " << oMinX << "," << oMinY << "," << oMaxX
             << "," << oMaxY;

        // Max/min zoom level
        tMaxZ = mercator.zoomForPixelSize(outGt[1]);
        tMinZ = mercator.zoomForPixelSize(outGt[1] *
                                          std::max(GDALGetRasterXSize(inputDataset),
                                                   GDALGetRasterYSize(inputDataset)) /
                                          tileSize);

        LOGD << "MinZ: " << tMinZ;
        LOGD << "MaxZ: " << tMaxZ;
        LOGD << "Num bands: " << nBands;
    }

    GDALTiler::~GDALTiler()
    {
        if (inputDataset && inputDataset != origDataset)
            GDALClose(inputDataset);
        if (origDataset)
            GDALClose(origDataset);
    }

    bool GDALTiler::tileWindow(int tz, int tx, int ty, GQResult &g)
    {
        BoundingBox<Projected2Di> tMinMax = getMinMaxCoordsForZ(tz);
        if (!tMinMax.contains(tx, ty))
        {
            LOGD << "Tile (" << tz << "," << tx << "," << ty << ") is out of bounds; emitting blank tile";
            return false;
        }

        const BoundingBox<Projected2D> b = mercator.tileBounds(tx, ty, tz);
        g = geoQuery(inputDataset, b.min.x, b.max.y, b.max.x, b.min.y, tileSize);

        LOGD << "GeoQuery: " << g.r.x << "," << g.r.y << "|" << g.r.xsize << "x"
             << g.r.ysize << "|" << g.w.x << "," << g.w.y << "|" << g.w.xsize << "x"
             << g.w.ysize;

        // A tile on the edge of the raster can have zero overlap after rounding: it is
        // emitted transparent so that tiling a whole zoom level does not abort
        return g.r.xsize > 0 && g.r.ysize > 0 && g.w.xsize > 0 && g.w.ysize > 0;
    }

    BandStatistics GDALTiler::bandStatistics(int band)
    {
        // The statistics of the file, not of the warped VRT
        const GDALDatasetH ds = origDataset != nullptr ? origDataset : inputDataset;
        return RasterStatsProvider::instance().get(inputPath, ds, band);
    }

    void GDALTiler::readBands(const GQResult &g, int bands, RasterImage &image)
    {
        const GDALDataType type = GDALGetRasterDataType(GDALGetRasterBand(inputDataset, 1));

        if (type == GDT_Byte || type == GDT_Unknown)
        {
            // Straight into the tile planes
            if (GDALDatasetRasterIO(inputDataset, GF_Read, g.r.x, g.r.y, g.r.xsize, g.r.ysize,
                                    image.band(0) + static_cast<size_t>(g.w.y) * image.width + g.w.x,
                                    g.w.xsize, g.w.ysize, GDT_Byte, bands, nullptr,
                                    1, image.width, static_cast<GSpacing>(image.pixels())) != CE_None)
                throw GDALException("Cannot read input dataset window");
            return;
        }

        const size_t windowPixels = static_cast<size_t>(g.w.xsize) * g.w.ysize;
        std::unique_ptr<uint8_t[]> buffer(new uint8_t[GDALGetDataTypeSizeBytes(type) * bands * windowPixels]);
        if (GDALDatasetRasterIO(inputDataset, GF_Read, g.r.x, g.r.y, g.r.xsize, g.r.ysize, buffer.get(),
                                g.w.xsize, g.w.ysize, type, bands, nullptr, 0, 0, 0) != CE_None)
            throw GDALException("Cannot read input dataset window");

        // One stretch for all bands, from the statistics of the raster
        // TODO: allow people to specify rescale values
        double globalMin = std::numeric_limits<double>::max();
        double globalMax = std::numeric_limits<double>::lowest();
        for (int i = 0; i < bands; i++)
        {
            const BandStatistics s = bandStatistics(i + 1);
            globalMin = std::min(globalMin, s.min);
            globalMax = std::max(globalMax, s.max);
        }

        switch (type)
        {
        case GDT_UInt16:
            rescaleInto<uint16_t>(buffer.get(), bands, g, globalMin, globalMax, image);
            break;
        case GDT_Int16:
            rescaleInto<int16_t>(buffer.get(), bands, g, globalMin, globalMax, image);
            break;
        case GDT_UInt32:
            rescaleInto<uint32_t>(buffer.get(), bands, g, globalMin, globalMax, image);
            break;
        case GDT_Int32:
            rescaleInto<int32_t>(buffer.get(), bands, g, globalMin, globalMax, image);
            break;
        case GDT_Float32:
            rescaleInto<float>(buffer.get(), bands, g, globalMin, globalMax, image);
            break;
        case GDT_Float64:
            rescaleInto<double>(buffer.get(), bands, g, globalMin, globalMax, image);
            break;
        default:
            break;
        }
    }

    void GDALTiler::readAlpha(const GQResult &g, RasterImage &image)
    {
        GDALRasterBandH alphaBand = FindAlphaBand(inputDataset);
        if (alphaBand == nullptr)
            alphaBand = GDALGetMaskBand(GDALGetRasterBand(inputDataset, 1));

        if (GDALRasterIO(alphaBand, GF_Read, g.r.x, g.r.y, g.r.xsize, g.r.ysize,
                         image.alpha.data() + static_cast<size_t>(g.w.y) * image.width + g.w.x,
                         g.w.xsize, g.w.ysize, GDT_Byte, 1, image.width) != CE_None)
            throw GDALException("Cannot read input dataset alpha window");
    }

    std::string GDALTiler::tile(int tz, int tx, int ty, ImageFormat format, uint8_t **outBuffer, int *outBufferSize)
    {
        const int xyzY = tms ? tmsToXYZ(ty, tz) : ty;

        // PNG holds at most 4 bands (RGBA)
        const int colorBands = std::max(1, std::min(3, nBands));
        RasterImage image(tileSize, tileSize, colorBands, true);

        GQResult g;
        if (tileWindow(tz, tx, xyzY, g))
        {
            readBands(g, colorBands, image);
            readAlpha(g, image);
        }

        return writeTile(image, format, tz, tx, ty, outBuffer, outBufferSize);
    }

    std::string GDALTiler::tile(int tz, int tx, int ty, const std::string &outputFormat, uint8_t **outBuffer,
                                int *outBufferSize)
    {
        return tile(tz, tx, ty, parseImageFormat(outputFormat), outBuffer, outBufferSize);
    }

    std::string GDALTiler::tile(int tz, int tx, int ty, const ThumbVisParams &visParams, uint8_t **outBuffer,
                                int *outBufferSize)
    {
        return tile(tz, tx, ty, visParams, ImageFormat::Png, outBuffer, outBufferSize);
    }

    std::string GDALTiler::tile(int tz, int tx, int ty, const ThumbVisParams &visParams, ImageFormat format,
                                uint8_t **outBuffer, int *outBufferSize)
    {
        // If no vis params set, fall through to standard tile
        if (visParams.preset.empty() && visParams.bands.empty() && visParams.formula.empty())
            return tile(tz, tx, ty, format, outBuffer, outBufferSize);

        const int xyzY = tms ? tmsToXYZ(ty, tz) : ty;
        if (!getMinMaxCoordsForZ(tz).contains(tx, xyzY))
            throw GDALException("Out of bounds");

        GQResult g;
        if (!tileWindow(tz, tx, xyzY, g))
        {
            LOGD << "Geoquery produced empty window (visParams); emitting transparent tile";
            return writeTile(RasterImage(tileSize, tileSize, 3, true), format, tz, tx, ty, outBuffer,
                             outBufferSize);
        }

        if (!visParams.formula.empty())
        {
            RasterImage image(tileSize, tileSize, 3, true);
            renderFormula(g, visParams, image);
            return writeTile(image, format, tz, tx, ty, outBuffer, outBufferSize);
        }

        // Band selection: explicit, from a preset or from the detected sensor
        const std::vector<int> selectedBands = resolveVisBands(inputPath, visParams, nBands);

        const int outBands = std::max(1, std::min(3, selectedBands.empty() ? nBands
                                                                           : static_cast<int>(selectedBands.size())));
        RasterImage image(tileSize, tileSize, outBands, true);
        if (selectedBands.empty())
            readBands(g, outBands, image);
        else
            readSelectedBands(g, std::vector<int>(selectedBands.begin(), selectedBands.begin() + outBands), image);
        readAlpha(g, image);

        return writeTile(image, format, tz, tx, ty, outBuffer, outBufferSize);
    }

    void GDALTiler::readSelectedBands(const GQResult &g, const std::vector<int> &bands, RasterImage &image)
    {
        const GDALDataType type = GDALGetRasterDataType(GDALGetRasterBand(inputDataset, 1));
        const size_t windowPixels = static_cast<size_t>(g.w.xsize) * g.w.ysize;
        std::vector<float> fBuf;
        std::vector<uint8_t> byteBuf(windowPixels);

        for (size_t i = 0; i < bands.size(); i++)
        {
            const int srcBand = bands[i];
            const GDALRasterBandH hSrcBand = GDALGetRasterBand(inputDataset, srcBand);

            if (type != GDT_Byte)
            {
                // Read as float, stretch over mean ± 2.33 σ (≈ p2-p98) clipped to min/max
                fBuf.resize(windowPixels);
                if (GDALRasterIO(hSrcBand, GF_Read, g.r.x, g.r.y, g.r.xsize, g.r.ysize,
                                 fBuf.data(), g.w.xsize, g.w.ysize, GDT_Float32, 0, 0) != CE_None)
                    throw GDALException("Cannot read band " + std::to_string(srcBand));

                const auto [sMin, sMax] = stretchRange(bandStatistics(srcBand));
                const double range = sMax - sMin;
                for (size_t px = 0; px < windowPixels; px++)
                {
                    const double v = std::max(sMin, std::min(sMax, static_cast<double>(fBuf[px])));
                    byteBuf[px] = static_cast<uint8_t>(255.0 * (v - sMin) / range);
                }
            }
            else if (GDALRasterIO(hSrcBand, GF_Read, g.r.x, g.r.y, g.r.xsize, g.r.ysize,
                                  byteBuf.data(), g.w.xsize, g.w.ysize, GDT_Byte, 0, 0) != CE_None)
            {
                throw GDALException("Cannot read band " + std::to_string(srcBand));
            }

            placeWindow(byteBuf.data(), g, image.band(static_cast<int>(i)), image.width);
        }
    }

    void GDALTiler::renderFormula(const GQResult &g, const ThumbVisParams &visParams, RasterImage &image)
    {
        const RasterWindow window{g.r.x, g.r.y, g.r.xsize, g.r.ysize, g.w.xsize, g.w.ysize};
        const auto rgba = renderFormulaRgba(inputDataset, inputPath, window, visParams);

        const size_t wSize = window.outPixels();
        std::vector<uint8_t> channel(wSize);
        for (int ch = 0; ch < 4; ch++)
        {
            for (size_t i = 0; i < wSize; i++)
                channel[i] = rgba[i * 4 + ch];
            placeWindow(channel.data(), g, ch < 3 ? image.band(ch) : image.alpha.data(), image.width);
        }
    }

    GDALDatasetH GDALTiler::createWarpedVRT(const GDALDatasetH &src,
                                            const OGRSpatialReferenceH &srs,
                                            GDALResampleAlg resampling)
    {
        char *dstWkt = nullptr;
        if (OSRExportToWkt(srs, &dstWkt) != OGRERR_NONE)
            throw GDALException("Cannot export dst WKT " + inputPath +
                                ". Is PROJ available?");
        const char *srcWkt = GDALGetProjectionRef(src);

        GDALWarpOptions *opts = GDALCreateWarpOptions();

        // If the dataset does not have alpha, add it
        bool hasAlpha = FindAlphaBand(src) != nullptr;
        if (!hasAlpha)
        {
            opts->nDstAlphaBand = GDALGetRasterCount(src) + 1;
        }

        const GDALDatasetH warpedVrt = GDALAutoCreateWarpedVRT(
            src, srcWkt, dstWkt, resampling, 0.001, opts);

        CPLFree(dstWkt);

        if (warpedVrt == nullptr)
        {
            GDALDestroyWarpOptions(opts);
            throw GDALException("Cannot create warped VRT");
        }

        GDALDestroyWarpOptions(opts);

        return warpedVrt;
    }

    GQResult GDALTiler::geoQuery(GDALDatasetH ds, double ulx, double uly, double lrx,
                                 double lry, int querySize)
    {
        GQResult o;
        double geo[6];
        if (GDALGetGeoTransform(ds, geo) != CE_None)
            throw GDALException("Cannot fetch geotransform geo");

        // Check for division by zero
        if (std::abs(geo[1]) < std::numeric_limits<double>::epsilon() ||
            std::abs(geo[5]) < std::numeric_limits<double>::epsilon())
        {
            throw GDALException("Invalid geotransform: pixel size is zero");
        }

        o.r.x = static_cast<int>((ulx - geo[0]) / geo[1] + 0.001);
        o.r.y = static_cast<int>((uly - geo[3]) / geo[5] + 0.001);
        o.r.xsize = static_cast<int>((lrx - ulx) / geo[1] + 0.5);
        o.r.ysize = static_cast<int>((lry - uly) / geo[5] + 0.5);

        if (querySize == 0)
        {
            o.w.xsize = o.r.xsize;
            o.w.ysize = o.r.ysize;
        }
        else
        {
            o.w.xsize = querySize;
            o.w.ysize = querySize;
        }

        o.w.x = 0;
        if (o.r.x < 0)
        {
            const int rxShift = std::abs(o.r.x);
            if (o.r.xsize > 0)
            {
                o.w.x = static_cast<int>(o.w.xsize * (static_cast<double>(rxShift) /
                                                      static_cast<double>(o.r.xsize)));
                o.w.xsize = o.w.xsize - o.w.x;
                o.r.xsize =
                    o.r.xsize -
                    static_cast<int>(o.r.xsize * (static_cast<double>(rxShift) /
                                                  static_cast<double>(o.r.xsize)));
            }
            o.r.x = 0;
        }

        const int rasterXSize = GDALGetRasterXSize(ds);
        const int rasterYSize = GDALGetRasterYSize(ds);

        if (o.r.x + o.r.xsize > rasterXSize)
        {
            if (o.r.xsize > 0)
            {
                o.w.xsize = static_cast<int>(
                    o.w.xsize *
                    (static_cast<double>(rasterXSize) - static_cast<double>(o.r.x)) /
                    static_cast<double>(o.r.xsize));
            }
            o.r.xsize = rasterXSize - o.r.x;
        }

        o.w.y = 0;
        if (o.r.y < 0)
        {
            const int ryShift = std::abs(o.r.y);
            if (o.r.ysize > 0)
            {
                o.w.y = static_cast<int>(o.w.ysize * (static_cast<double>(ryShift) /
                                                      static_cast<double>(o.r.ysize)));
                o.w.ysize = o.w.ysize - o.w.y;
                o.r.ysize =
                    o.r.ysize -
                    static_cast<int>(o.r.ysize * (static_cast<double>(ryShift) /
                                                  static_cast<double>(o.r.ysize)));
            }
            o.r.y = 0;
        }

        if (o.r.y + o.r.ysize > rasterYSize)
        {
            if (o.r.ysize > 0)
            {
                o.w.ysize = static_cast<int>(
                    o.w.ysize *
                    (static_cast<double>(rasterYSize) - static_cast<double>(o.r.y)) /
                    static_cast<double>(o.r.ysize));
            }
            o.r.ysize = rasterYSize - o.r.y;
        }

        return o;
    }

    GDALRasterBandH GDALTiler::FindAlphaBand(const GDALDatasetH &dataset)
    {
        // If the dataset does not have alpha, add it
        const int numBands = GDALGetRasterCount(dataset);
        for (int n = 0; n < numBands; n++)
        {
            GDALRasterBandH b = GDALGetRasterBand(dataset, n + 1);
            if (GDALGetRasterColorInterpretation(b) == GCI_AlphaBand)
            {
                return b;
            }
        }
        return nullptr;
    }

} // namespace ddb
