/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "dsm_reader.h"

#include "exceptions.h"
#include "artefact_cache.h"
#include "gdal_raii.h"
#include "logger.h"
#include "mio.h"

namespace ddb
{

    DsmRaster readDsmRaster(const fs::path &filePath, double latitude, double longitude)
    {
        // Owned: the checks below throw while the dataset is open
        const auto dataset = raster::openInputRaster(io::toUtf8(filePath));
        if (!dataset)
            throw GDALException("Cannot open " + io::toUtf8(filePath));
        GDALDatasetH hDataset = datasetHandle(dataset);

        DsmRaster raster;
        DSMCacheEntry &e = raster.entry;
        e.width = static_cast<unsigned int>(GDALGetRasterXSize(hDataset));
        e.height = static_cast<unsigned int>(GDALGetRasterYSize(hDataset));

        if (GDALGetGeoTransform(hDataset, e.geoTransform) != CE_None)
            throw GDALException("Cannot get geotransform for " + io::toUtf8(filePath));

        const std::string wkt = GDALGetProjectionRef(hDataset);
        if (wkt.empty())
            throw GDALException("Cannot get projection ref for " + io::toUtf8(filePath));

        // TODO: support for DSM with EPSG different than 4326
        if (GDALGetRasterCount(hDataset) != 1)
            throw GDALException("More than 1 raster band found in elevation raster: " + io::toUtf8(filePath));

        GDALRasterBandH hBand = GDALGetRasterBand(hDataset, 1);
        e.nodata = readBandNodata(hBand);

        Point2D min(0, e.height);
        Point2D max(e.width, 0);
        min.transform(e.geoTransform);
        max.transform(e.geoTransform);

        e.bbox.min = min;
        e.bbox.max = max;
        const Point2D position(longitude, latitude);
        raster.containsPoint = e.bbox.contains(position);

        if (raster.containsPoint)
        {
            // Inside the boundaries, load data
            LOGD << position << " inside raster boundary, loading data from " << io::toUtf8(filePath);
            e.loadData(hDataset);
        }
        return raster;
    }

} // namespace ddb

void DSMCacheEntry::loadData(GDALDatasetH hDataset)
{
    GDALRasterBandH hBand = GDALGetRasterBand(hDataset, 1);
    data.clear();
    data.assign(width * height, 0.0f);
    if (GDALRasterIO(hBand, GF_Read, 0, 0, static_cast<int>(width),
                     static_cast<int>(height),
                     &data[0],
                     static_cast<int>(width),
                     static_cast<int>(height),
                     GDT_Float32, 0, 0) != CE_None)
    {
        throw ddb::GDALException("Cannot read raster data");
    }
}

float DSMCacheEntry::getElevation(double latitude, double longitude)
{
    // TODO: This could be empty if the DSM is not loaded, we should understand why this could happen
    if (data.empty())
        throw ddb::AppException("Cannot get elevation, need to call loadData() first.");
    if (width == 0 || height == 0)
        throw ddb::AppException("Cannot get elevation, need to populate width/height first.");

    double originX, originY, pixelSizeX, pixelSizeY;
    originX = geoTransform[0];
    originY = geoTransform[3];
    pixelSizeX = geoTransform[1];
    pixelSizeY = geoTransform[5];

    unsigned int pixelX = static_cast<unsigned int>((longitude - originX) / pixelSizeX);
    unsigned int pixelY = static_cast<unsigned int>((latitude - originY) / pixelSizeY);

    if (pixelX > width || pixelY > height)
    {
        throw ddb::AppException("Pixel coordinates (" +
                                std::to_string(pixelX) + "," + std::to_string(pixelY) + ") " +
                                "are outside of raster boundaries (" + std::to_string(width) + "x" + std::to_string(height) + ")");
    }

    return data[pixelX + pixelY * width];
}
