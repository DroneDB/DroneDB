/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DEM_H
#define DEM_H

#include <memory>
#include <string>

#include "coordstransformer.h"
#include "ddb_export.h"
#include "gdal_inc.h"
#include "gdal_raii.h"
#include "raster_utils.h"

namespace ddb
{
    // An opened elevation raster (DEM): the one setup that the volume, stockpile, profile and
    // contour analyses share. Movable, not copyable (it owns the dataset).
    struct DemContext
    {
        GDALDatasetUniquePtr dataset;
        GDALRasterBandH band = nullptr; // the band that was asked for, owned by the dataset
        double gt[6] = {0, 1, 0, 0, 0, 1};
        int width = 0;
        int height = 0;
        BandNodata nodata;              // of `band`
        std::string projection;         // the WKT of the raster, "" when it has none
        SrsPtr srs;                     // null when the raster has no (readable) CRS
        bool isGeographic = false;      // the CRS is geographic: pixel sizes are in degrees

        GDALDatasetH handle() const noexcept { return GDALDataset::ToHandle(dataset.get()); }

        // WGS84 (longitude, latitude) to the CRS of the raster, and back. Null when the raster
        // has no CRS: its map coordinates are taken as WGS84 then. Throw GDALException when
        // GDAL cannot build the transformation.
        DDB_DLL std::unique_ptr<CoordsTransformer> wgs84ToDem() const;
        DDB_DLL std::unique_ptr<CoordsTransformer> demToWgs84() const;
    };

    // Opens `path` for reading and checks that it can be used as a DEM: it has a geotransform and
    // the band `bandIndex` (1-based). Throws GDALException ("Cannot open raster: ...", "Raster has
    // no geotransform: ...", "Raster has no bands") and InvalidArgsException ("Invalid band index:
    // ...").
    DDB_DLL DemContext openDem(const std::string &path, int bandIndex = 1);
}

#endif // DEM_H
