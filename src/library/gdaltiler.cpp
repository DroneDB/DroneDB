/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gdaltiler.h"

#include <mutex>
#include <memory>
#include <vector>

#include "entry.h"
#include "exceptions.h"
#include "logger.h"
#include "mio.h"

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

    GDALTiler::GDALTiler(const std::string &inputPath, const std::string &outputFolder,
                         int tileSize, bool tms)
        : Tiler(inputPath, outputFolder, tileSize, tms)
    {

        pngDrv = GDALGetDriverByName("PNG");
        if (pngDrv == nullptr)
            throw GDALException("Cannot create PNG driver");
        memDrv = GDALGetDriverByName("MEM");
        if (memDrv == nullptr)
            throw GDALException("Cannot create MEM driver");

        std::string openPath = inputPath;
        if (utils::isNetworkPath(openPath))
        {
            CPLSetConfigOption("GDAL_DISABLE_READDIR_ON_OPEN", "YES");
            CPLSetConfigOption("CPL_VSIL_CURL_ALLOWED_EXTENSIONS", ".tif,.tiff");
            // CPLSetConfigOption("CPL_DEBUG", "ON");

            openPath = "/vsicurl/" + openPath;
        }

        inputDataset = GDALOpen(openPath.c_str(), GA_ReadOnly);
        if (inputDataset == nullptr)
            throw GDALException("Cannot open " + openPath);

        rasterCount = GDALGetRasterCount(inputDataset);
        if (rasterCount == 0)
            throw GDALException("No raster bands found in " + openPath);

        // Extract no data values
        //    std::vector<double> inNodata;
        //    int success;
        //    for (int i = 0; i < rasterCount; i++){
        //        GDALRasterBandH band = GDALGetRasterBand(inputDataset, i + 1);
        //        double nodata = GDALGetRasterNoDataValue(band, &success);
        //        if (success) inNodata.push_back(nodata);
        //    }

        // Extract input SRS
        const OGRSpatialReferenceH inputSrs = OSRNewSpatialReference(nullptr);
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
        if (OSRImportFromWkt(inputSrs, &wktp) != OGRERR_NONE)
            throw GDALException("Cannot read spatial reference system for " + openPath + ". Is PROJ available?");

        // Setup output SRS
        const OGRSpatialReferenceH outputSrs = OSRNewSpatialReference(nullptr);
        OSRImportFromEPSG(outputSrs, 3857); // TODO: support for geodetic?

        if (!hasGeoreference(inputDataset))
            throw GDALException(openPath + " is not georeferenced.");

        // Check if we need to reproject
        if (!sameProjection(inputSrs, outputSrs))
        {
            origDataset = inputDataset;
            inputDataset = createWarpedVRT(inputDataset, outputSrs);
        }

        OSRDestroySpatialReference(inputSrs);
        OSRDestroySpatialReference(outputSrs);

        // TODO: nodata?
        // if (inNodata.size() > 0){
        //        update_no_data_values
        //}

        // warped_input_dataset = inputDataset
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

    std::string GDALTiler::tile(int tz, int tx, int ty, uint8_t **outBuffer, int *outBufferSize)
    {
        std::string tilePath = getTilePath(tz, tx, ty, true);

        if (tms)
        {
            ty = tmsToXYZ(ty, tz);
            LOGD << "TY: " << ty;
        }

        BoundingBox<Projected2Di> tMinMax = getMinMaxCoordsForZ(tz);
        if (!tMinMax.contains(tx, ty))
            throw GDALException("Out of bounds");

        // Need to create in-memory dataset
        // (PNG driver does not have Create() method)
        int cappedBands = std::min(3, nBands); // PNG driver supports at most 4 bands (rgba)
        const GDALDatasetH dsTile = GDALCreate(memDrv, "", tileSize, tileSize, cappedBands + 1,
                                               GDT_Byte, nullptr);
        if (dsTile == nullptr)
            throw GDALException("Cannot create dsTile");

        BoundingBox<Projected2D> b = mercator.tileBounds(tx, ty, tz);

        const int querySize = tileSize; // TODO: you will need to change this for
                                        // interpolations other than NN
        GQResult g = geoQuery(inputDataset, b.min.x, b.max.y, b.max.x, b.min.y, querySize);

        LOGD << "GeoQuery: " << g.r.x << "," << g.r.y << "|" << g.r.xsize << "x"
             << g.r.ysize << "|" << g.w.x << "," << g.w.y << "|" << g.w.xsize << "x"
             << g.w.ysize;

        if (g.r.xsize != 0 && g.r.ysize != 0 && g.w.xsize != 0 && g.w.ysize != 0)
        {
            const GDALDataType type =
                GDALGetRasterDataType(GDALGetRasterBand(inputDataset, 1));

            const size_t wSize = g.w.xsize * g.w.ysize;
            std::unique_ptr<uint8_t[]> buffer(
                new uint8_t[GDALGetDataTypeSizeBytes(type) * cappedBands * wSize]);

            if (GDALDatasetRasterIO(inputDataset, GF_Read, g.r.x, g.r.y, g.r.xsize,
                                    g.r.ysize, buffer.get(), g.w.xsize, g.w.ysize, type,
                                    cappedBands, nullptr, 0, 0, 0) != CE_None)
            {
                throw GDALException("Cannot read input dataset window");
            }

            // Rescale if needed
            // We currently don't rescale byte datasets
            // TODO: allow people to specify rescale values

            if (type != GDT_Byte && type != GDT_Unknown)
            {
                std::unique_ptr<uint8_t[]> scaledBuffer(new uint8_t[GDALGetDataTypeSizeBytes(GDT_Byte) * cappedBands * wSize]);
                size_t bufSize = wSize * cappedBands;

                double globalMin = std::numeric_limits<double>::max(),
                       globalMax = std::numeric_limits<double>::min();

                for (int i = 0; i < cappedBands; i++)
                {
                    double bMin, bMax;

                    GDALDatasetH ds = origDataset != nullptr ? origDataset : inputDataset; // Use the actual dataset, not the VRT
                    GDALDatasetH hBand = GDALGetRasterBand(ds, i + 1);

                    CPLErr statsRes = GDALGetRasterStatistics(hBand, TRUE, FALSE, &bMin, &bMax, nullptr, nullptr);
                    if (statsRes == CE_Warning)
                    {
                        double bMean, bStdDev;
                        if (GDALGetRasterStatistics(hBand, TRUE, TRUE, &bMin, &bMax, &bMean, &bStdDev) != CE_None)
                            throw GDALException("Cannot compute band statistics (forced)");
                        if (GDALSetRasterStatistics(hBand, bMin, bMax, bMean, bStdDev) != CE_None)
                            throw GDALException("Cannot cache band statistics");

                        LOGD << "Cached band " << i << " statistics (" << bMin << ", " << bMax << ")";
                    }
                    else if (statsRes == CE_Failure)
                    {
                        throw GDALException("Cannot compute band statistics");
                    }

                    globalMin = std::min(globalMin, bMin);
                    globalMax = std::max(globalMax, bMax);
                }

                switch (type)
                {
                case GDT_Byte:
                    rescale<uint8_t>(buffer.get(), scaledBuffer.get(), bufSize, globalMin, globalMax);
                    break;
                case GDT_UInt16:
                    rescale<uint16_t>(buffer.get(), scaledBuffer.get(), bufSize, globalMin, globalMax);
                    break;
                case GDT_Int16:
                    rescale<int16_t>(buffer.get(), scaledBuffer.get(), bufSize, globalMin, globalMax);
                    break;
                case GDT_UInt32:
                    rescale<uint32_t>(buffer.get(), scaledBuffer.get(), bufSize, globalMin, globalMax);
                    break;
                case GDT_Int32:
                    rescale<int32_t>(buffer.get(), scaledBuffer.get(), bufSize, globalMin, globalMax);
                    break;
                case GDT_Float32:
                    rescale<float>(buffer.get(), scaledBuffer.get(), bufSize, globalMin, globalMax);
                    break;
                case GDT_Float64:
                    rescale<double>(buffer.get(), scaledBuffer.get(), bufSize, globalMin, globalMax);
                    break;
                default:
                    break;
                }

                buffer = std::move(scaledBuffer);
            }

            const GDALRasterBandH raster = GDALGetRasterBand(inputDataset, 1);
            GDALRasterBandH alphaBand = FindAlphaBand(inputDataset);
            if (alphaBand == nullptr)
                alphaBand = GDALGetMaskBand(raster);

            std::unique_ptr<uint8_t[]> alphaBuffer(
                new uint8_t[GDALGetDataTypeSizeBytes(GDT_Byte) * wSize]);
            if (GDALRasterIO(alphaBand, GF_Read, g.r.x, g.r.y, g.r.xsize, g.r.ysize,
                             alphaBuffer.get(), g.w.xsize, g.w.ysize, GDT_Byte, 0,
                             0) != CE_None)
            {
                throw GDALException("Cannot read input dataset alpha window");
            }

            // Write data

            if (tileSize == querySize)
            {
                if (GDALDatasetRasterIO(dsTile, GF_Write, g.w.x, g.w.y, g.w.xsize,
                                        g.w.ysize, buffer.get(), g.w.xsize, g.w.ysize,
                                        GDT_Byte, cappedBands, nullptr, 0, 0,
                                        0) != CE_None)
                {
                    throw GDALException("Cannot write tile data");
                }

                LOGD << "Wrote tile data";

                const GDALRasterBandH tileAlphaBand =
                    GDALGetRasterBand(dsTile, cappedBands + 1);
                GDALSetRasterColorInterpretation(tileAlphaBand, GCI_AlphaBand);

                if (GDALRasterIO(tileAlphaBand, GF_Write, g.w.x, g.w.y, g.w.xsize,
                                 g.w.ysize, alphaBuffer.get(), g.w.xsize, g.w.ysize,
                                 GDT_Byte, 0, 0) != CE_None)
                {
                    throw GDALException("Cannot write tile alpha data");
                }

                LOGD << "Wrote tile alpha";
            }
            else
            {
                // TODO: readraster query in memory scaled to tilesize
                throw GDALException("Not implemented");
            }
        }
        else
        {
            throw GDALException("Geoquery out of bounds");
        }

        const GDALDatasetH outDs = GDALCreateCopy(pngDrv, tilePath.c_str(), dsTile, FALSE,
                                                  nullptr, nullptr, nullptr);
        if (outDs == nullptr)
            throw GDALException("Cannot create output dataset " + tilePath);

        GDALFlushCache(outDs);
        GDALClose(outDs);
        GDALClose(dsTile);

        if (outBuffer != nullptr)
        {
            vsi_l_offset bufSize;
            *outBuffer = VSIGetMemFileBuffer(tilePath.c_str(), &bufSize, TRUE);
            if (bufSize > std::numeric_limits<int>::max())
                throw GDALException("Exceeded max buf size");
            *outBufferSize = bufSize;
            return "";
        }
        else
        {
            return tilePath;
        }
    }

    template <typename T>
    void GDALTiler::rescale(uint8_t *buffer, uint8_t *dstBuffer, size_t bufsize, double bMin, double bMax)
    {
        T *ptr = reinterpret_cast<T *>(buffer);

        // Avoid divide by zero
        if (bMin == bMax)
            bMax += 0.1;

        LOGD << "Min: " << bMin << " | Max: " << bMax;

        // Can still happen according to GDAL for very large values
        if (bMin == bMax)
            throw GDALException(
                "Cannot scale values due to source min/max being equal");

        double deltamm = bMax - bMin;

        for (size_t i = 0; i < bufsize; i++)
        {
            double v = std::max(bMin, std::min(bMax, static_cast<double>(ptr[i])));
            dstBuffer[i] = static_cast<uint8_t>(255.0 * (v - bMin) / deltamm);
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
