/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "dsm_reader.h"

#include <algorithm>

#include "exceptions.h"
#include "logger.h"
#include "mio.h"

namespace ddb
{

    DsmRaster readDsmRaster(const fs::path &filePath, double latitude, double longitude)
    {
        const auto file = tilers::Raster::openInput(io::toUtf8(filePath));
        if (!file)
            throw GDALException("Cannot open " + io::toUtf8(filePath));

        DsmRaster raster;
        DSMCacheEntry &e = raster.entry;
        e.width = static_cast<unsigned int>(file->width());
        e.height = static_cast<unsigned int>(file->height());

        if (!file->geoTransform())
            throw GDALException("Cannot get geotransform for " + io::toUtf8(filePath));
        std::copy(file->geoTransform()->begin(), file->geoTransform()->end(), e.geoTransform);

        if (file->wkt().empty())
            throw GDALException("Cannot get projection ref for " + io::toUtf8(filePath));

        // TODO: support for DSM with EPSG different than 4326

        if (file->bandCount() != 1)
            throw GDALException("More than 1 raster band found in elevation raster: " + io::toUtf8(filePath));

        if (const auto nodata = file->band(1).nodata)
            e.nodata = {true, *nodata};

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
            e.loadData(*file);
        }

        return raster;
    }

} // namespace ddb

void DSMCacheEntry::loadData(const ddb::tilers::Raster &raster)
{
    data.assign(static_cast<size_t>(width) * height, 0.0f);
    const int band = 1;
    raster.read(0, 0, static_cast<int>(width), static_cast<int>(height), static_cast<int>(width),
                static_cast<int>(height), std::span<const int>(&band, 1), data.data());
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
