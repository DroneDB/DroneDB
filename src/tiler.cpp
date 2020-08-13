/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <vector>
#include <memory>
#include "tiler.h"
#include "exceptions.h"

namespace ddb{

bool Tiler::hasGeoreference(const GDALDatasetH &dataset){
    double geo[6] = {0.0, 1.0, 0.0,
                     0.0, 0.0, 1.0};
    GDALGetGeoTransform(dataset, geo);
    return (geo[0] != 0.0 && geo[1] != 1.0 && geo[2] != 0.0 &&
            geo[3] != 0.0 && geo[4] != 1.0 && geo[5] != 1.0) || GDALGetGCPCount(dataset) != 0;
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

Tiler::Tiler(const std::string &geotiffPath) : geotiffPath(geotiffPath)
{

}

std::string Tiler::tile(int x, int y, int z)
{
    GDALDriverH outDrv = GDALGetDriverByName( "PNG" );
    if (outDrv == nullptr) throw GDALException("Cannot create PNG driver");
    GDALDriverH memDrv = GDALGetDriverByName( "MEM" );
    if (memDrv == nullptr) throw GDALException("Cannot create PNG driver");

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
        // TODO: create VRT with new projection
        // and replace inputDataset with it
        // https://github.com/Luqqk/gdal2tiles/blob/master/gdal2tiles.py#L738
    }



    OSRDestroySpatialReference(inputSrs);
    OSRDestroySpatialReference(outputSrs);

}

}
