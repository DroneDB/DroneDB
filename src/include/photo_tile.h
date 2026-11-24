/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef PHOTO_TILE_H
#define PHOTO_TILE_H

#include <array>

#include "geo.h"
#include "raster_protocol.h"

namespace ddb::raster
{
    // Where a photo lies on the web map: plain arithmetic on its georeference (the corners of its
    // footprint), shared by the host, which decides which tiles show the photo without reading it,
    // and the raster helper, which draws them. The photo is placed on the ground by its corners as
    // ground control points (a homography, as the COG builds of a photo place it), so its edges are
    // the straight lines between the corners of its footprint.

    // The corners of the footprint in EPSG:3857: upper left, lower left, lower right, upper right
    std::array<Projected2D, 4> photoCornersInMeters(const PhotoGeoreference &photo);

    // The EPSG:3857 box that holds the footprint
    BoundingBox<Projected2D> photoBoundsInMeters(const PhotoGeoreference &photo);

    // EPSG:3857 metres per pixel of the photo on the ground, as gdalwarp suggests the resolution of
    // a warp: the ground distance from the corner (0, 0) to the corner (width, height) over the
    // diagonal of the photo in pixels
    double photoResolution(const PhotoGeoreference &photo);

    // The EPSG:3857 box of the tile of `params` (its row read as params.tms says)
    BoundingBox<Projected2D> photoTileBounds(const PhotoTileParams &params);

    // Whether some of the footprint lies in `box` grown by `margin` metres on each side: a tile
    // that does not hold any of it shows nothing of the photo. The edges of the footprint are
    // straight in EPSG:4326 and bend a little in EPSG:3857 (under a millimetre over a kilometre):
    // the margin covers that.
    bool footprintMeetsBox(const PhotoGeoreference &photo, const BoundingBox<Projected2D> &box, double margin);

} // namespace ddb::raster

#endif // PHOTO_TILE_H
