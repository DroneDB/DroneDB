/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DEM_H
#define DEM_H

#include <cmath>
#include <memory>
#include <optional>
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

    // How a band is read as values (FR-XC-24): the NoData value that replaces the one of the band
    struct BandValueOptions
    {
        std::optional<double> nodata; // overrides the NoData value of the band (FR-RAS-31)
    };

    // An opened elevation raster (DEM): the one setup that the volume, stockpile, profile and
    // contour analyses share, and the one band-value model of every raster value the Hub shows
    // (point, area, profile, contours, value info, statistics): the value of a pixel is
    // raw * scale + offset, and a pixel has no value when it is NoData (the override, else the NoData
    // value of the band), masked (alpha band or dataset mask) or not finite.
    // Movable, not copyable (it owns the dataset).
    struct DemContext
    {
        GDALDatasetUniquePtr dataset;
        GDALRasterBandH band = nullptr; // the band that was asked for, owned by the dataset
        double gt[6] = {0, 1, 0, 0, 0, 1};
        bool hasGeoTransform = true;    // false for a raster opened without one (openDem with requireGeo false): gt is the identity
        int width = 0;
        int height = 0;
        BandNodata nodata;              // of `band`: the override when there is one, else the NoData value of the band
        bool nodataOverridden = false;  // `nodata` is the override of the caller
        bool nodataInvalid = false;     // the NoData value of the band cannot be held by its type: no pixel can match it
        GDALRasterBandH maskBand = nullptr; // the alpha band or dataset mask of `band` (not a NoData mask), null when none
        bool hasScale = false;          // the band declares a scale and/or an offset
        double scale = 1.0;             // physical = raw * scale + offset
        double offset = 0.0;
        std::string unit;               // the unit type of the band, "" when it has none
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

        // The physical value of a raw sample: raw * scale + offset (the sample itself when the band has none)
        double toPhysical(double raw) const noexcept { return hasScale ? raw * scale + offset : raw; }
        // The raw sample of a physical value (the inverse of toPhysical; the value itself for a scale of 0)
        double toRaw(double physical) const noexcept
        {
            return hasScale && scale != 0.0 ? (physical - offset) / scale : physical;
        }

        // Whether the reads below hand out NaN for a pixel with no value (and not the NoData value of
        // the band): they do whenever they convert the samples
        bool readsNormalised() const noexcept
        {
            return zToMeters != 1.0 || hasScale || nodataOverridden || maskBand != nullptr;
        }

        // Whether a sample is an elevation: finite, and not the nodata of the band
        bool isValid(float v) const noexcept
        {
            return std::isfinite(v) && (readsNormalised() || !isNodata(v, nodata));
        }

        // The window (x, y, w, h) of the band, resampled to bufferW x bufferH, as the raw samples of the
        // band (floats; NaN where a pixel has no value: NoData, masked or not finite). Throws
        // AppException("Cannot read raster data") when GDAL cannot read it.
        std::vector<float> readRaw(int x, int y, int w, int h, int bufferW, int bufferH) const;
        // The same in physical values (raw * scale + offset, the unit of the band)
        std::vector<float> readValues(int x, int y, int w, int h, int bufferW, int bufferH) const;
        std::vector<float> readValues(int x, int y, int w, int h) const { return readValues(x, y, w, h, w, h); }

        // The fractional pixel (column, row) of a WGS84 coordinate: the one geo-to-pixel conversion of the
        // raster values. The point is moved to the CRS of the raster (a raster with no CRS takes it as
        // WGS84), then converted with the inverse geotransform. Throws AppException when the point
        // cannot be transformed.
        void wgs84ToPixel(double lon, double lat, double &px, double &py) const;
        // The pixel that holds the WGS84 coordinate (floor; negative or beyond the size when outside)
        void wgs84ToPixelIndex(double lon, double lat, int &col, int &row) const;

        // The pixels under a WGS84 rectangle (minLon, minLat, maxLon, maxLat): those whose centre is inside it,
        // as inclusive bounds (x0, y0, x1, y1) that may lie partly or wholly outside the raster. The sides of
        // the rectangle are moved to the CRS of the raster point by point (a rectangle of the map is a curved
        // shape in another CRS), then to pixels with wgs84ToPixel. A rectangle smaller than a pixel holds the
        // pixel under its centre. A rectangle with minLon > maxLon runs across the antimeridian: it is the box east
        // of minLon over 180 to maxLon on a raster that spans the antimeridian, and refused with an
        // InvalidArgsException on any other raster (never swapped into the other side of the Earth). Throws
        // AppException when a point cannot be transformed.
        void wgs84BoxToPixelBounds(double minLon, double minLat, double maxLon, double maxLat, int &x0, int &y0,
                                   int &x1, int &y1) const;

        // The window (x, y, w, h) of the band, resampled to bufferW x bufferH, as floats, in meters
        // (the values of readValues times zToMeters; a pixel with no value reads back as NaN). Throws
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

    // The same with the options of the values (the NoData override), and for a caller that
    // reads pixels of a raster that has no geotransform (an ungeoreferenced raster; its gt is the
    // identity) with requireGeo false
    DemContext openDem(const std::string &path, int bandIndex, const BandValueOptions &options,
                       bool requireGeo = true);
}

#endif // DEM_H
