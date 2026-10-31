/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DEM_H
#define DEM_H

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "coordstransformer.h"
#include "gdal_inc.h"
#include "gdal_raii.h"
#include "raster_utils.h"

namespace ddb
{
    // The size of a pixel on the ground, in meters
    struct PixelSize
    {
        double x = 0;
        double y = 0;
    };

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
        double zToMeters = 1.0;         // the factor of the elevations of `band` to meters (its unit type)

        GDALDatasetH handle() const noexcept { return GDALDataset::ToHandle(dataset.get()); }

        // WGS84 (longitude, latitude) to the CRS of the raster, and back. Null when the raster
        // has no CRS: its map coordinates are taken as WGS84 then. Throw GDALException when
        // GDAL cannot build the transformation.
        std::unique_ptr<CoordsTransformer> wgs84ToDem() const;
        std::unique_ptr<CoordsTransformer> demToWgs84() const;

        // Whether a sample is an elevation: finite, and not the nodata of the band
        bool isValid(float v) const noexcept { return std::isfinite(v) && !isNodata(v, nodata); }

        // The window (x, y, w, h) of the band, resampled to bufferW x bufferH, as floats, in meters
        // (see zToMeters; the nodata of a band that is converted reads back as NaN). Throws
        // AppException("Cannot read raster data") when GDAL cannot read it.
        std::vector<float> readElevations(int x, int y, int w, int h, int bufferW, int bufferH) const;
        std::vector<float> readElevations(int x, int y, int w, int h) const { return readElevations(x, y, w, h, w, h); }

        // The size of a pixel on the ground, in meters, at the map coordinate (x, y) of the raster.
        // A geographic CRS (or none) has its geotransform in degrees, converted with a local WGS84
        // approximation at the latitude y. Any other CRS has it in its own linear unit (meters, feet,
        // US survey feet) and distorts it with the projection (Web Mercator stretches the ground by
        // 1 / cos(latitude)): the pixel is moved to the ellipsoid and measured there, so x * y is its
        // true area. Throws AppException when the pixel cannot be measured.
        PixelSize pixelSizeMeters(double x, double y) const;
    };

    // The factor from the elevations of a raster band to meters, read from its unit type ("ft",
    // "foot", "US survey foot", ...): 1 for meters, an unset unit and any unit that is not a length
    double elevationUnitToMeters(const std::string &unitType);

    // Opens `path` for reading and checks that it can be used as a DEM: it has a geotransform and
    // the band `bandIndex` (1-based). Throws GDALException ("Cannot open raster: ...", "Raster has
    // no geotransform: ...", "Raster has no bands") and InvalidArgsException ("Invalid band index:
    // ...").
    DemContext openDem(const std::string &path, int bandIndex = 1);
}

#endif // DEM_H
