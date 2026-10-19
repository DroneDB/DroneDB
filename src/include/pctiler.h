/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef PCTILER_H
#define PCTILER_H

#include "gdal_inc.h"

#include <sstream>
#include <string>

#include "ddb_export.h"
#include "fs.h"
#include "geo.h"
#include "tiler.h"
#include "pointcloud.h"
#include "coordstransformer.h"

#include <memory>

namespace ddb
{

    // Tiler that renders web-map raster tiles from a built COPC (.copc.laz) file
    // using PDAL's CopcReader. Drop-in replacement for the previous EptTiler.
    // The constructor reads the COPC header and prepares the coordinate transforms once;
    // every tile then streams the points of its window through PointRasterizer. An
    // instance must not be used by two threads at once (RasterCache hands out one per
    // caller).
    class PointCloudTiler : public Tiler
    {
        PointCloudInfo pcInfo;
        bool hasColors;
        std::unique_ptr<CoordsTransformer> toCloud;    // EPSG:3857 -> cloud CRS
        std::unique_ptr<CoordsTransformer> toMercator; // cloud CRS -> EPSG:3857

    public:
        DDB_DLL PointCloudTiler(const std::string &copcPath,
                                const std::string &outputFolder, int tileSize = 256,
                                bool tms = false);
        DDB_DLL ~PointCloudTiler();

        using Tiler::tile;

        DDB_DLL std::string tile(int tz, int tx, int ty, ImageFormat format,
                                 uint8_t **outBuffer = nullptr, int *outBufferSize = nullptr) override;

        // Tiles are read from the COPC level this many zoom levels coarser than the tile's
        // pixel size (DDB_PCTILE_RESOLUTION_OFFSET, default 2)
        DDB_DLL static int resolutionOffset();
    };

} // namespace ddb

#endif // PCTILER_H
