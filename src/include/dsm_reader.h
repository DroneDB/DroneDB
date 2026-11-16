/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef DSM_READER_H
#define DSM_READER_H

// Reads a DSM raster (one band, georeferenced) into memory: the part of the DSM service
// (dsmservice.h) that knows about the file and the layout of the pixels. The DSM is read with
// ddb-tilers (tilers_raster.h), as GDAL reads it.

#include <cstring>
#include <vector>

#include "fs.h"
#include "geo.h"
#include "raster_utils.h"
#include "tilers_raster.h"

struct DSMCacheEntry
{
    unsigned int width, height;
    ddb::BandNodata nodata;

    double geoTransform[6];
    std::vector<float> data;
    ddb::BoundingBox<ddb::Point2D> bbox;

    DSMCacheEntry() : width(0), height(0)
    {
        memset(geoTransform, 0, sizeof(double) * 6);
    }

    // Reads every elevation of band 1 of `raster` as Float32
    void loadData(const ddb::tilers::Raster &raster);
    float getElevation(double latitude, double longitude);
};

namespace ddb
{

    // A DSM file read for a point
    struct DsmRaster
    {
        DSMCacheEntry entry;
        bool containsPoint = false; // the raster covers the point, so entry.data is loaded
    };

    // Opens the DSM at filePath and reads its georeferencing; the pixels are read only when the
    // raster covers the point (longitude, latitude), so a file outside the query costs one open.
    // Throws GDALException when the file cannot be opened, has no geotransform or projection,
    // or has more than one band.
    DsmRaster readDsmRaster(const fs::path &filePath, double latitude, double longitude);

} // namespace ddb

#endif // DSM_READER_H
