/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <vector>
#include <memory>
#include "tiler.h"
#include "exceptions.h"
#include "logger.h"

namespace ddb{

bool Tiler::hasGeoreference(const GDALDatasetH &dataset){
    double geo[6] = {0.0, 1.0, 0.0,
                     0.0, 0.0, 1.0};
    if (GDALGetGeoTransform(dataset, geo) != CE_None) throw GDALException("Cannot fetch geotransform in hasGeoreference");

    return (geo[0] != 0.0 || geo[1] != 1.0 || geo[2] != 0.0 ||
            geo[3] != 0.0 || geo[4] != 0.0 || geo[5] != 1.0) || GDALGetGCPCount(dataset) != 0;
}

bool Tiler::sameProjection(const OGRSpatialReferenceH &a, const OGRSpatialReferenceH &b){
    char *aProj;
    char *bProj;

    if (OSRExportToProj4(a, &aProj) != CE_None) throw GDALException("Cannot export proj4");
    if (OSRExportToProj4(b, &bProj) != CE_None) throw GDALException("Cannot export proj4");

    bool same = std::string(aProj) == std::string(bProj);

    delete aProj;
    delete bProj;
    return same;
}

int Tiler::dataBandsCount(const GDALDatasetH &dataset){
    GDALRasterBandH raster = GDALGetRasterBand(dataset, 1);
    GDALRasterBandH alphaBand = GDALGetMaskBand(raster);
    if ((GDALGetMaskFlags(alphaBand) & GMF_ALPHA) ||
            GDALGetRasterCount(dataset) == 4 ||
            GDALGetRasterCount(dataset) == 2){
        return GDALGetRasterCount(dataset) - 1;
    }else{
        // TODO: does this work for multispectral TIFFs (B > 4)?
        return GDALGetRasterCount(dataset);
    }
}

std::string Tiler::getTilePath(int z, int x, int y, bool createIfNotExists){
    // TODO: retina tiles support?
    fs::path dir = outputFolder / std::to_string(z) / std::to_string(x);
    if (createIfNotExists && !fs::exists(dir)){
        if (!fs::create_directories(dir)) throw FSException("Cannot create " + dir.string());
    }

    fs::path p = dir / fs::path(std::to_string(y) + ".png");
    return p.string();
}

Tiler::Tiler(const std::string &geotiffPath, const std::string &outputFolder) :
    geotiffPath(geotiffPath), outputFolder(outputFolder)
{
    tileSize = 256; // TODO: dynamic?
}

std::string Tiler::tile(int tz, int tx, int ty, bool tms)
{
    GDALDriverH pngDrv = GDALGetDriverByName( "PNG" );
    if (pngDrv == nullptr) throw GDALException("Cannot create PNG driver");
    GDALDriverH memDrv = GDALGetDriverByName( "MEM" );
    if (memDrv == nullptr) throw GDALException("Cannot create MEM driver");

    GDALDatasetH inputDataset;
    inputDataset = GDALOpen( geotiffPath.c_str(), GA_ReadOnly );
    if( inputDataset == nullptr ) throw GDALException("Cannot open " + geotiffPath);

    int rasterCount = GDALGetRasterCount(inputDataset);
    if (rasterCount == 0) throw GDALException("No raster bands found in " + geotiffPath);

    // Extract no data values
    std::vector<double> inNodata;
    int success;
    for (int i = 0; i < rasterCount; i++){
        GDALRasterBandH band = GDALGetRasterBand(inputDataset, i + 1);
        double nodata = GDALGetRasterNoDataValue(band, &success);
        if (success) inNodata.push_back(nodata);
    }

    // Extract input SRS
    OGRSpatialReferenceH inputSrs = OSRNewSpatialReference(nullptr);
    std::string inputSrsWkt;
    if (GDALGetProjectionRef(inputDataset) != NULL){
        inputSrsWkt = GDALGetProjectionRef(inputDataset);
    }else if (GDALGetGCPCount(inputDataset) > 0){
        inputSrsWkt = GDALGetGCPProjection(inputDataset);
    }else{
        throw GDALException("No projection found in " + geotiffPath);
    }

    char *wktp = const_cast<char *>(inputSrsWkt.c_str());
    if (OSRImportFromWkt(inputSrs, &wktp) != OGRERR_NONE){
        throw GDALException("Cannot read spatial reference system for " + geotiffPath + ". Is PROJ available?");
    }

    // Setup output SRS
    OGRSpatialReferenceH outputSrs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(outputSrs, 3857); // TODO: support for geodetic?

    if (!hasGeoreference(inputDataset)) throw GDALException(geotiffPath + " is not georeferenced.");

    // Check if we need to reproject
    if (!sameProjection(inputSrs, outputSrs)){
        inputDataset = createWarpedVRT(inputDataset, outputSrs);
    }

    // TODO: nodata?
    //if (inNodata.size() > 0){
//        update_no_data_values
    //}

    // warped_input_dataset = inputDataset
    int nBands = dataBandsCount(inputDataset);

    double outGt[6];
    if (GDALGetGeoTransform(inputDataset, outGt) != CE_None) throw GDALException("Cannot fetch geotransform outGt");


    double oMinX = outGt[0];
    double oMaxX = outGt[0] + GDALGetRasterXSize(inputDataset) * outGt[1];
    double oMaxY = outGt[3];
    double oMinY = outGt[3] - GDALGetRasterYSize(inputDataset) * outGt[1];

    LOGD << "Bounds (output SRS): " << oMinX << "," << oMinY << "," << oMaxX << "," << oMaxY;

    GlobalMercator mercator;

    // Maximal zoom level
    int tMaxZ = mercator.zoomForPixelSize(outGt[1]);

    BoundingBox<Projected2D> tMinMax(
        mercator.metersToTile(oMinX, oMinY, tz),
        mercator.metersToTile(oMaxX, oMaxY, tz)
    );

    // TODO: crop tiles extending world limits?

    int tMinZ = mercator.zoomForPixelSize(outGt[1] * std::max(GDALGetRasterXSize(inputDataset), GDALGetRasterYSize(inputDataset)) / tileSize);
    if (tz < tMinZ) throw GDALException("Requested tile zoom too low (minimum is: " + std::to_string(tMinZ));

    LOGD << "MinZ: " << tMinZ;
    LOGD << "MaxZ: " << tMaxZ;
    LOGD << "Num bands: " << nBands;

    std::string tilePath = getTilePath(tz, tx, ty, true);

    // Need to create in-memory dataset
    // (PNG driver does not have Create() method)
    GDALDatasetH dsTile = GDALCreate(memDrv, "", tileSize, tileSize, nBands, GDT_Byte, nullptr);
    if (dsTile == nullptr) throw GDALException("Cannot create dsTile");

    if (!tms){
        ty = tmsToXYZ(ty, tz);
    }
    BoundingBox<Projected2D> b = mercator.tileBounds(tx, ty, tz);

    GQResult g = geoQuery(inputDataset, b.min.x, b.max.y, b.max.x, b.min.y);
    int nativeSize = g.w.x + g.w.xsize;
    int querySize = tileSize; // TODO: you will need to change this for interpolations other than NN
    g = geoQuery(inputDataset, b.min.x, b.max.y, b.max.x, b.min.y, querySize);

    LOGD << g.r.x << ", " << g.r.y << "\n" <<
            g.r.xsize << "x" << g.r.ysize << "\n" <<
            g.w.x << ", " << g.w.y << "\n" <<
            g.w.xsize << "x" << g.w.ysize;

    if (g.r.xsize != 0 && g.r.ysize != 0 && g.w.xsize != 0 and g.w.ysize != 0){
        GDALDataType type = GDALGetRasterDataType(GDALGetRasterBand(inputDataset, 1));

        int bufsize = GDALGetDataTypeSizeBytes(type) *
                      nBands *
                      g.w.xsize * g.w.ysize;
        LOGD << "Bufsize: " << bufsize;
        char *buffer = new char[bufsize];

        if (GDALDatasetRasterIO(inputDataset, GF_Read, g.r.x, g.r.y, g.r.xsize, g.r.ysize,
                            buffer, g.w.xsize, g.w.ysize, type, nBands, nullptr, 0, 0, 0) != CE_None){
            throw GDALException("Cannot read input dataset window");
        }

        LOGD << "Read input dataset";

        // TODO: handle alpha

        if (tileSize == querySize){
            if (GDALDatasetRasterIO(dsTile, GF_Write, 0, 0, g.w.xsize, g.w.ysize,
                                buffer, g.w.xsize, g.w.ysize, type, nBands, nullptr, 0, 0, 0) != CE_None){
                throw GDALException("Cannot write tile data");
            }
        }else{
            // TODO: readraster query in memory scaled to tilesize
            throw GDALException("Not implemented");
        }

        delete[] buffer;
    }else{
        throw GDALException("Out of bounds");
    }

    GDALDatasetH outDs = GDALCreateCopy(pngDrv, tilePath.c_str(), dsTile, FALSE, nullptr, nullptr, nullptr);
    if (outDs == nullptr) throw GDALException("Cannot create output dataset " + tilePath);

//            if (tMinMax.contains(x, y)){
//                std::string tilePath = getTilePath(x, y, tz, true);

//                GDALOpen(tilePath.c_str(), )
//            }

    // https://github.com/mapbox/rasterio/blob/master/rasterio/vrt.py
    // https://github.com/mapbox/rasterio/blob/42d3a9d3ac9deaf22024b5c796f4ab703c87a302/rasterio/_warp.pyx#L722
    // https://github.com/mapbox/rasterio/blob/925718ea5cc61eaca46dcdb03739ecdde18f8e21/rasterio/_io.pyx#L1665

    OSRDestroySpatialReference(inputSrs);
    OSRDestroySpatialReference(outputSrs);

    GDALClose(dsTile);
    GDALClose(outDs);
    GDALClose(inputDataset);

    return tilePath;
}

GDALDatasetH Tiler::createWarpedVRT(const GDALDatasetH &src, const OGRSpatialReferenceH &srs, GDALResampleAlg resampling){
    //GDALDriverH vrtDrv = GDALGetDriverByName( "VRT" );
    //if (vrtDrv == nullptr) throw GDALException("Cannot create VRT driver");

    //std::string vrtFilename = "/vsimem/" + uuidv4() + ".vrt";
    //GDALDatasetH vrt = GDALCreateCopy(vrtDrv, vrtFilename.c_str(), src, FALSE, nullptr, nullptr, nullptr);

    char *dstWkt;
    if (OSRExportToWkt(srs, &dstWkt) != OGRERR_NONE) throw GDALException("Cannot export dst WKT " + geotiffPath + ". Is PROJ available?");
    const char *srcWkt = GDALGetProjectionRef(src);

    GDALDatasetH warpedVrt = GDALAutoCreateWarpedVRT(src, srcWkt, dstWkt, resampling, 0.001, nullptr);
    if (warpedVrt == nullptr) throw GDALException("Cannot create warped VRT");

    delete dstWkt;

    return warpedVrt;
}

GQResult Tiler::geoQuery(GDALDatasetH ds, double ulx, double uly, double lrx, double lry, int querySize){
    GQResult o;
    double geo[6];
    if (GDALGetGeoTransform(ds, geo) != CE_None) throw GDALException("Cannot fetch geotransform geo");

    o.r.x = static_cast<int>((ulx - geo[0]) / geo[1] + 0.001);
    o.r.y = static_cast<int>((uly - geo[3]) / geo[5] + 0.001);
    o.r.xsize = static_cast<int>((lrx - ulx) / geo[1] + 0.5);
    o.r.ysize = static_cast<int>((lry - uly) / geo[5] + 0.5);

    if (querySize == 0){
        o.w.xsize = o.r.xsize;
        o.w.ysize = o.r.ysize;
    }else{
        o.w.xsize = querySize;
        o.w.ysize = querySize;
    }

    o.w.x = 0;
    if (o.r.x < 0){
        int rxShift = std::abs(o.r.x);
        o.w.x = static_cast<int>(o.w.xsize * (static_cast<double>(rxShift) / static_cast<double>(o.r.xsize)));
        o.w.xsize = o.w.xsize - o.w.x;
        o.r.xsize = o.r.xsize - static_cast<int>(o.r.xsize * (static_cast<double>(rxShift) / static_cast<double>(o.r.xsize)));
        o.r.x = 0;
    }

    int rasterXSize = GDALGetRasterXSize(ds);
    int rasterYSize = GDALGetRasterYSize(ds);

    if (o.r.x + o.r.xsize > rasterXSize){
        o.w.xsize = static_cast<int>(o.w.xsize * (static_cast<double>(rasterXSize - o.r.x) / static_cast<double>(o.r.xsize)));
        o.r.xsize = rasterXSize - o.r.x;
    }

    o.w.y = 0;
    if (o.r.y < 0){
        int ryShift = std::abs(o.r.y);
        o.w.y = static_cast<int>(o.w.ysize * (static_cast<double>(ryShift) / static_cast<double>(o.r.ysize)));
        o.w.ysize = o.w.ysize - o.w.y;
        o.r.ysize = o.r.ysize - static_cast<int>(o.r.ysize * (static_cast<double>(ryShift) / static_cast<double>(o.r.ysize)));
        o.r.y = 0;
    }

    if (o.r.y + o.r.ysize > rasterYSize){
        o.w.ysize = static_cast<int>(o.w.ysize * (static_cast<double>(rasterYSize - o.r.y) / static_cast<double>(o.r.ysize)));
        o.r.ysize = rasterYSize - o.r.y;
    }

    return o;
}

int Tiler::tmsToXYZ(int ty, int tz){
    return (std::pow(2, tz) - 1) - ty;
}

GlobalMercator::GlobalMercator(){
    tileSize = 256; // TODO: dynamic?
    originShift = 2.0 * M_PI * 6378137.0 / 2.0;
    initialResolution = 2.0 * M_PI * 6378137.0 / static_cast<double>(tileSize);
    maxZoomLevel = 99;
}

BoundingBox<Geographic2D> GlobalMercator::tileLatLonBounds(int tx, int ty, int zoom){
    BoundingBox<Projected2D> bounds = tileBounds(tx, ty, zoom);
    Geographic2D min = metersToLatLon(bounds.min.x, bounds.min.y);
    Geographic2D max = metersToLatLon(bounds.max.x, bounds.max.y);
    return BoundingBox<Geographic2D>(min, max);
}

BoundingBox<Projected2D> GlobalMercator::tileBounds(int tx, int ty, int zoom){
    Projected2D min = pixelsToMeters(tx * tileSize, ty * tileSize, zoom);
    Projected2D max = pixelsToMeters((tx + 1) * tileSize, (ty + 1) * tileSize, zoom);
    return BoundingBox<Projected2D>(min, max);
}

Geographic2D GlobalMercator::metersToLatLon(int mx, int my){
    double lon = static_cast<double>(mx) / originShift * 180.0;
    double lat = static_cast<double>(my) / originShift * 180.0;

    lat = 180.0 / M_PI * (2 * std::atan(std::exp(lat * M_PI / 180.0)) - M_PI / 2.0);
    return Geographic2D(lon, lat);
}

Projected2D GlobalMercator::metersToTile(int mx, int my, int zoom){
    Projected2D p = metersToPixels(mx, my, zoom);
    return pixelsToTile(p.x, p.y);
}

Projected2D GlobalMercator::pixelsToMeters(int px, int py, int zoom){
    double res = resolution(zoom);
    return Projected2D(px * res - originShift, py * res - originShift);
}

Projected2D GlobalMercator::metersToPixels(int mx, int my, int zoom){
    double res = resolution(zoom);
    return Projected2D((mx + originShift) / res, (my + originShift) / res);
}

Projected2D GlobalMercator::pixelsToTile(int px, int py){
    return Projected2D(
        static_cast<int>(std::ceil(static_cast<double>(px) / static_cast<double>(tileSize)) - 1),
        static_cast<int>(std::ceil(static_cast<double>(py) / static_cast<double>(tileSize)) - 1)
    );
}

double GlobalMercator::resolution(int zoom){
    return initialResolution / std::pow(2, zoom);
}

int GlobalMercator::zoomForPixelSize(double pixelSize){
    for (int i = 0; i < maxZoomLevel; i++){
        if (pixelSize > resolution(i)){
            return i - 1;
        }
    }
    LOGW << "Exceeded max zoom level";
    return 0;
}


}
