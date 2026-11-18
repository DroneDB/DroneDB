/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef POLAR_FOOTPRINT_H
#define POLAR_FOOTPRINT_H

#include <functional>
#include <optional>

#include "gdal_inc.h"
#include "pointcloud_crs.h"

namespace ddb
{
    // Maps a point in place between a grid and the CRS the grid is placed in; false when it cannot
    using GridMapping = std::function<bool(double &x, double &y)>;

    // A box of a grid placed in a CRS: the pixels [0, width] x [0, height] of a raster with its
    // geotransform (or the transform of its ground control points) both ways, or a box of the CRS
    // itself (the extent of a vector layer, the bounds of a point cloud), whose mappings are empty
    struct GridBox
    {
        double minX = 0, minY = 0, maxX = 0, maxY = 0;
        GridMapping toCrs;   // grid -> CRS; empty when the grid is the CRS
        GridMapping fromCrs; // CRS -> grid; empty when the grid is the CRS
    };

    // The footprint of a box that holds a geographic pole, as a longitude/latitude envelope in the
    // degrees of `lonLat` (a geographic CRS: WGS 84 for every footprint of an entry).
    //
    // A footprint made of the corners of a box is wrong around a pole: the four corners of a polar
    // stereographic raster centred on the South Pole are at one latitude (31.7 S for an Antarctic
    // raster), and their ring is a band that leaves the pole and the whole of the raster out. As GDAL
    // finds a pole in a box (OCTTransformBounds, GDALSuggestedWarpOutput): the pole is in the box when
    // it transforms from `lonLat` into `crs`, then into the grid, within [minX, maxX] x [minY, maxY]
    // (1e-5 of a grid unit, a pixel of a raster, more). The footprint is then every longitude, from
    // that pole to the most equatorward latitude of the edges of the box (each side sampled at 21
    // points between its corners, as GDAL densifies them); both poles give the whole globe.
    //
    // Nothing (the ring of the corners stays the footprint) when the box holds no pole, when a corner
    // of the box is at the pole it holds (that ring passes through the pole already: a geographic
    // raster that reaches latitude 90, a quadrant of a polar grid), when GDAL cannot transform between
    // the two CRSs, or when no point of the edges transforms.
    std::optional<LonLatEnvelope> polarEnvelope(const GridBox &box, const OGRSpatialReference &crs,
                                                const OGRSpatialReference &lonLat);
}

#endif // POLAR_FOOTPRINT_H
