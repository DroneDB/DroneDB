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

namespace ddb {

bool GDALTiler::hasGeoreference(const GDALDatasetH &dataset) {
    double geo[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    if (GDALGetGeoTransform(dataset, geo) != CE_None)
        throw GDALException("Cannot fetch geotransform in hasGeoreference");

    return (geo[0] != 0.0 || geo[1] != 1.0 || geo[2] != 0.0 || geo[3] != 0.0 ||
            geo[4] != 0.0 || geo[5] != 1.0) ||
           GDALGetGCPCount(dataset) != 0;
}

bool GDALTiler::sameProjection(const OGRSpatialReferenceH &a,
                           const OGRSpatialReferenceH &b) {
    char *aProj;
    char *bProj;

    if (OSRExportToProj4(a, &aProj) != CE_None)
        throw GDALException("Cannot export proj4");
    if (OSRExportToProj4(b, &bProj) != CE_None)
        throw GDALException("Cannot export proj4");

    return std::string(aProj) == std::string(bProj);
}

int GDALTiler::dataBandsCount(const GDALDatasetH &dataset) {
    const GDALRasterBandH raster = GDALGetRasterBand(dataset, 1);
    const GDALRasterBandH alphaBand = GDALGetMaskBand(raster);
    const int bandsCount = GDALGetRasterCount(dataset);

    if (GDALGetMaskFlags(alphaBand) & GMF_ALPHA || bandsCount == 4 ||
        bandsCount == 2) {
        return bandsCount - 1;
    }

    return bandsCount;
}

GDALTiler::GDALTiler(const std::string &inputPath, const std::string &outputFolder,
             int tileSize, bool tms)
    : Tiler(inputPath, outputFolder, tileSize, tms) {

    pngDrv = GDALGetDriverByName("PNG");
    if (pngDrv == nullptr) throw GDALException("Cannot create PNG driver");
    memDrv = GDALGetDriverByName("MEM");
    if (memDrv == nullptr) throw GDALException("Cannot create MEM driver");

    std::string openPath = inputPath;
    if (utils::isNetworkPath(openPath)){
        CPLSetConfigOption("GDAL_DISABLE_READDIR_ON_OPEN", "YES");
        CPLSetConfigOption("CPL_VSIL_CURL_ALLOWED_EXTENSIONS", ".tif,.tiff");
        //CPLSetConfigOption("CPL_DEBUG", "ON");

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
    if (GDALGetProjectionRef(inputDataset) != nullptr) {
        inputSrsWkt = GDALGetProjectionRef(inputDataset);
    } else if (GDALGetGCPCount(inputDataset) > 0) {
        inputSrsWkt = GDALGetGCPProjection(inputDataset);
    } else {
        throw GDALException("No projection found in " + openPath);
    }

    char *wktp = const_cast<char *>(inputSrsWkt.c_str());
    if (OSRImportFromWkt(inputSrs, &wktp) != OGRERR_NONE) {
        throw GDALException("Cannot read spatial reference system for " +
                            openPath + ". Is PROJ available?");
    }
    OSRSetAxisMappingStrategy(inputSrs, OSRAxisMappingStrategy::OAMS_TRADITIONAL_GIS_ORDER);

    // Setup output SRS
    const OGRSpatialReferenceH outputSrs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(outputSrs, 3857);  // TODO: support for geodetic?

    if (!hasGeoreference(inputDataset))
        throw GDALException(openPath + " is not georeferenced.");

    // Check if we need to reproject
    if (!sameProjection(inputSrs, outputSrs)) {
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
    //    bool hasAlpha = nBands < GDALGetRasterCount(inputDataset);

    double outGt[6];
    if (GDALGetGeoTransform(inputDataset, outGt) != CE_None)
        throw GDALException("Cannot fetch geotransform outGt");

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

GDALTiler::~GDALTiler() {
    if (inputDataset) GDALClose(inputDataset);
    if (origDataset) GDALClose(origDataset);
}

std::string GDALTiler::tile(int tz, int tx, int ty, uint8_t **outBuffer, int *outBufferSize){
    std::string tilePath = getTilePath(tz, tx, ty, true);

    if (tms) {
        ty = tmsToXYZ(ty, tz);
        LOGD << "TY: " << ty;
    }

    BoundingBox<Projected2Di> tMinMax = getMinMaxCoordsForZ(tz);
    if (!tMinMax.contains(tx, ty)) throw GDALException("Out of bounds");

    // Need to create in-memory dataset
    // (PNG driver does not have Create() method)
    const GDALDatasetH dsTile = GDALCreate(memDrv, "", tileSize, tileSize, nBands + 1,
                                           GDT_Byte, nullptr);
    if (dsTile == nullptr) throw GDALException("Cannot create dsTile");

    BoundingBox<Projected2D> b = mercator.tileBounds(tx, ty, tz);

    GQResult g = geoQuery(inputDataset, b.min.x, b.max.y, b.max.x, b.min.y);
    // int nativeSize = g.w.x + g.w.xsize;
    const int querySize = tileSize;  // TODO: you will need to change this for
                               // interpolations other than NN
    g = geoQuery(inputDataset, b.min.x, b.max.y, b.max.x, b.min.y, querySize);

    LOGD << "GeoQuery: " << g.r.x << "," << g.r.y << "|" << g.r.xsize << "x"
         << g.r.ysize << "|" << g.w.x << "," << g.w.y << "|" << g.w.xsize << "x"
         << g.w.ysize;

    if (g.r.xsize != 0 && g.r.ysize != 0 && g.w.xsize != 0 && g.w.ysize != 0) {
        const GDALDataType type =
            GDALGetRasterDataType(GDALGetRasterBand(inputDataset, 1));

        const size_t wSize = g.w.xsize * g.w.ysize;
        char *buffer =
            new char[GDALGetDataTypeSizeBytes(type) * nBands * wSize];

        if (GDALDatasetRasterIO(inputDataset, GF_Read, g.r.x, g.r.y, g.r.xsize,
                                g.r.ysize, buffer, g.w.xsize, g.w.ysize, type,
                                nBands, nullptr, 0, 0, 0) != CE_None) {
            throw GDALException("Cannot read input dataset window");
        }

        // Rescale if needed
        // We currently don't rescale byte datasets
        // TODO: allow people to specify rescale values

        if (type != GDT_Byte && type != GDT_Unknown) {
            for (int i = 0; i < nBands; i++) {
                const GDALRasterBandH hBand = GDALGetRasterBand(inputDataset, i + 1);
                char *wBuf = (buffer + wSize * i);

                switch (type) {
                    case GDT_Byte:
                        rescale<uint8_t>(hBand, wBuf, wSize);
                        break;
                    case GDT_UInt16:
                        rescale<uint16_t>(hBand, wBuf, wSize);
                        break;
                    case GDT_Int16:
                        rescale<int16_t>(hBand, wBuf, wSize);
                        break;
                    case GDT_UInt32:
                        rescale<uint32_t>(hBand, wBuf, wSize);
                        break;
                    case GDT_Int32:
                        rescale<int32_t>(hBand, wBuf, wSize);
                        break;
                    case GDT_Float32:
                        rescale<float>(hBand, wBuf, wSize);
                        break;
                    case GDT_Float64:
                        rescale<double>(hBand, wBuf, wSize);
                        break;
                    default:
                        break;
                }
            }
        }

        const GDALRasterBandH raster = GDALGetRasterBand(inputDataset, 1);
        const GDALRasterBandH alphaBand = GDALGetMaskBand(raster);

        char *alphaBuffer =
            new char[GDALGetDataTypeSizeBytes(GDT_Byte) * wSize];
        if (GDALRasterIO(alphaBand, GF_Read, g.r.x, g.r.y, g.r.xsize, g.r.ysize,
                         alphaBuffer, g.w.xsize, g.w.ysize, GDT_Byte, 0,
                         0) != CE_None) {
            throw GDALException("Cannot read input dataset alpha window");
        }

        // Write data

        if (tileSize == querySize) {
            if (GDALDatasetRasterIO(dsTile, GF_Write, g.w.x, g.w.y, g.w.xsize,
                                    g.w.ysize, buffer, g.w.xsize, g.w.ysize,
                                    type, nBands, nullptr, 0, 0,
                                    0) != CE_None) {
                throw GDALException("Cannot write tile data");
            }

            LOGD << "Wrote tile data";

            const GDALRasterBandH tileAlphaBand =
                GDALGetRasterBand(dsTile, nBands + 1);
            GDALSetRasterColorInterpretation(tileAlphaBand, GCI_AlphaBand);

            if (GDALRasterIO(tileAlphaBand, GF_Write, g.w.x, g.w.y, g.w.xsize,
                             g.w.ysize, alphaBuffer, g.w.xsize, g.w.ysize,
                             GDT_Byte, 0, 0) != CE_None) {
                throw GDALException("Cannot write tile alpha data");
            }

            LOGD << "Wrote tile alpha";
        } else {
            // TODO: readraster query in memory scaled to tilesize
            throw GDALException("Not implemented");
        }

        delete[] buffer;
        delete[] alphaBuffer;
    } else {
        throw GDALException("Geoquery out of bounds");
    }

    const GDALDatasetH outDs = GDALCreateCopy(pngDrv, tilePath.c_str(), dsTile, FALSE,
                                              nullptr, nullptr, nullptr);
    if (outDs == nullptr)
        throw GDALException("Cannot create output dataset " + tilePath);

    GDALFlushCache(outDs);
    GDALClose(outDs);
    GDALClose(dsTile);

    if (outBuffer != nullptr){
        vsi_l_offset bufSize;
        *outBuffer = VSIGetMemFileBuffer(tilePath.c_str(), &bufSize, TRUE);
        if (bufSize > std::numeric_limits<int>::max()) throw GDALException("Exceeded max buf size");
        *outBufferSize = bufSize;
        return "";
    }else{
        return tilePath;
    }

}

template <typename T>
void GDALTiler::rescale(GDALRasterBandH hBand, char *buffer, size_t bufsize) {
    double minmax[2];
    GDALComputeRasterMinMax(hBand, TRUE, minmax);

    // Avoid divide by zero
    if (minmax[0] == minmax[1]) minmax[1] += 0.1;

    LOGD << "Min: " << minmax[0] << " | Max: " << minmax[1];

    // Can still happen according to GDAL for very large values
    if (minmax[0] == minmax[1])
        throw GDALException(
            "Cannot scale values due to source min/max being equal");

    double deltamm = minmax[1] - minmax[0];
    T *ptr = reinterpret_cast<T *>(buffer);

    for (size_t i = 0; i < bufsize; i++) {
        ptr[i] = static_cast<T>(((ptr[i] - minmax[0]) / deltamm) * 255.0);
    }
}

GDALDatasetH GDALTiler::createWarpedVRT(const GDALDatasetH &src,
                                    const OGRSpatialReferenceH &srs,
                                    GDALResampleAlg resampling) {
    char *dstWkt;
    if (OSRExportToWkt(srs, &dstWkt) != OGRERR_NONE)
        throw GDALException("Cannot export dst WKT " + inputPath +
                            ". Is PROJ available?");
    const char *srcWkt = GDALGetProjectionRef(src);

    const GDALDatasetH warpedVrt = GDALAutoCreateWarpedVRT(
        src, srcWkt, dstWkt, resampling, 0.001, nullptr);
    if (warpedVrt == nullptr) throw GDALException("Cannot create warped VRT");

    return warpedVrt;
}

GQResult GDALTiler::geoQuery(GDALDatasetH ds, double ulx, double uly, double lrx,
                         double lry, int querySize) {
    GQResult o;
    double geo[6];
    if (GDALGetGeoTransform(ds, geo) != CE_None)
        throw GDALException("Cannot fetch geotransform geo");

    o.r.x = static_cast<int>((ulx - geo[0]) / geo[1] + 0.001);
    o.r.y = static_cast<int>((uly - geo[3]) / geo[5] + 0.001);
    o.r.xsize = static_cast<int>((lrx - ulx) / geo[1] + 0.5);
    o.r.ysize = static_cast<int>((lry - uly) / geo[5] + 0.5);

    if (querySize == 0) {
        o.w.xsize = o.r.xsize;
        o.w.ysize = o.r.ysize;
    } else {
        o.w.xsize = querySize;
        o.w.ysize = querySize;
    }

    o.w.x = 0;
    if (o.r.x < 0) {
        const int rxShift = std::abs(o.r.x);
        o.w.x = static_cast<int>(o.w.xsize * (static_cast<double>(rxShift) /
                                              static_cast<double>(o.r.xsize)));
        o.w.xsize = o.w.xsize - o.w.x;
        o.r.xsize =
            o.r.xsize -
            static_cast<int>(o.r.xsize * (static_cast<double>(rxShift) /
                                          static_cast<double>(o.r.xsize)));
        o.r.x = 0;
    }

    const int rasterXSize = GDALGetRasterXSize(ds);
    const int rasterYSize = GDALGetRasterYSize(ds);

    if (o.r.x + o.r.xsize > rasterXSize) {
        o.w.xsize = static_cast<int>(
            o.w.xsize *
            (static_cast<double>(rasterXSize) - static_cast<double>(o.r.x)) /
            static_cast<double>(o.r.xsize));
        o.r.xsize = rasterXSize - o.r.x;
    }

    o.w.y = 0;
    if (o.r.y < 0) {
        const int ryShift = std::abs(o.r.y);
        o.w.y = static_cast<int>(o.w.ysize * (static_cast<double>(ryShift) /
                                              static_cast<double>(o.r.ysize)));
        o.w.ysize = o.w.ysize - o.w.y;
        o.r.ysize =
            o.r.ysize -
            static_cast<int>(o.r.ysize * (static_cast<double>(ryShift) /
                                          static_cast<double>(o.r.ysize)));
        o.r.y = 0;
    }

    if (o.r.y + o.r.ysize > rasterYSize) {
        o.w.ysize = static_cast<int>(
            o.w.ysize *
            (static_cast<double>(rasterYSize) - static_cast<double>(o.r.y)) /
            static_cast<double>(o.r.ysize));
        o.r.ysize = rasterYSize - o.r.y;
    }

    return o;
}


}  // namespace ddb
