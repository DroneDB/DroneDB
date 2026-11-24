/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "photo_tiler.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "cancellation.h"
#include "entry_handler.h"
#include "exceptions.h"
#include "geoproject.h"
#include "mathutil.h"
#include "mio.h"
#include "photo_tile.h"
#include "raster_artefacts.h"
#include "raster_helper.h"

namespace ddb
{
    namespace
    {
        // Runs `use` with a folder only this request writes to, which goes with everything in it when
        // `use` returns
        template <typename Use> void inRequestFolder(const Use &use)
        {
            const raster::RequestFolder request("photo-tile-");
            use(request.path());
        }

        // The transparent tile: what a tile that holds nothing of the photo shows
        RasterImage blankTile(int tileSize)
        {
            return RasterImage(tileSize, tileSize, 3, true);
        }
    } // namespace

    PhotoTiler::PhotoTiler(const fs::path &photo, const std::string &tilesFolder, int size, bool tmsRows,
                           const raster::PhotoGeoreference &georeference)
        : Tiler(io::toUtf8(photo), tilesFolder, size, tmsRows), georeference_(georeference)
    {
        // The grid of the COG gdalwarp writes in EPSG:3857 with TILING_SCHEME=GoogleMapsCompatible and
        // ZOOM_LEVEL_STRATEGY=UPPER: the first zoom level whose resolution is at least as fine as the one
        // gdalwarp suggests, and the extent snapped outwards to its 256 px tiles (by less than half a
        // pixel). The arithmetic is the builder's, operation for operation: the zoom levels of the tiles
        // come from the pixel size it computes, and a rounding of it decides the deepest one.
        const double resolution = raster::photoResolution(georeference_);
        const double half = 6378137.0 * ddb::kPi;
        const auto levelResolution = [half](int zoom) { return 2 * half / 256 / static_cast<double>(1u << zoom); };
        int zoom = 0;
        while (zoom <= 30 && !(resolution > levelResolution(zoom) ||
                               std::abs(resolution - levelResolution(zoom)) / levelResolution(zoom) <= 1e-8))
            zoom++;
        if (!std::isfinite(resolution) || zoom > 30)
            throw GDALException("Cannot tile " + inputPath + ": its footprint gives it no size on the ground");

        const double zres = levelResolution(zoom);
        const double tileExtent = zres * 256;
        const double eps = 0.499 * zres;
        const long matrix = 1L << zoom;
        const BoundingBox<Projected2D> box = raster::photoBoundsInMeters(georeference_);
        const long left = std::max(0L, static_cast<long>(std::floor((box.min.x + half + eps) / tileExtent)));
        const long top = std::max(0L, static_cast<long>(std::floor((half - box.max.y + eps) / tileExtent)));
        const long right = std::min(matrix, static_cast<long>(std::ceil((box.max.x + half - eps) / tileExtent)));
        const long bottom = std::min(matrix, static_cast<long>(std::ceil((half - box.min.y - eps) / tileExtent)));
        const double gMinX = -half + static_cast<double>(left) * tileExtent;
        const double gMaxX = -half + static_cast<double>(right) * tileExtent;
        const double gMaxY = half - static_cast<double>(top) * tileExtent;
        const long width = std::lround((gMaxX - gMinX) / zres);
        const long height = std::lround((gMaxY - (half - static_cast<double>(bottom) * tileExtent)) / zres);
        if (width <= 0 || height <= 0)
            throw GDALException("Cannot tile " + inputPath + ": its footprint is outside the web map");
        const double pixel = (gMaxX - gMinX) / static_cast<double>(width);

        // The bounds and the zoom levels of a raster of that grid, as ddb-tilers tells them of a COG
        oMinX = gMinX;
        oMaxX = gMinX + static_cast<double>(width) * pixel;
        oMaxY = gMaxY;
        oMinY = gMaxY - static_cast<double>(height) * pixel;
        tMaxZ = mercator.zoomForPixelSize(pixel);
        tMinZ = mercator.zoomForPixelSize(pixel * static_cast<double>(std::max(width, height)) / tileSize);
        nBands = 0; // not used: the helper chooses the bands of every tile
    }

    std::optional<BoundingBox<Projected2D>> PhotoTiler::contentBounds() const
    {
        return raster::photoBoundsInMeters(georeference_);
    }

    std::string PhotoTiler::tile(int tz, int tx, int ty, ImageFormat format, uint8_t **outBuffer, int *outBufferSize)
    {
        // Out of the world is out of the photo
        if (!tileInWorld(tz, tx, ty))
            return writeTile(blankTile(tileSize), format, tz, tx, ty, outBuffer, outBufferSize);

        raster::PhotoTileParams params;
        static_cast<raster::PhotoGeoreference &>(params) = georeference_;
        params.z = tz;
        params.x = tx;
        params.y = ty;
        params.tms = !tms; // the flag of a Tiler is true for XYZ rows
        params.tileSize = tileSize;
        params.format = format;

        // A tile that holds nothing of the footprint (a pixel of margin) is made here, without the helper
        if (!raster::footprintMeetsBox(georeference_, raster::photoTileBounds(params), mercator.resolution(tz)))
            return writeTile(blankTile(tileSize), format, tz, tx, ty, outBuffer, outBufferSize);

        cancel::throwIfCanceled();
        std::string bytes;
        inRequestFolder([&](const fs::path &folder) {
            const fs::path out = folder / (std::string("tile.") + imageFormatExtension(format));
            raster::rasterHelper().photoTile(io::fromUtf8(inputPath), out, params);
            bytes = raster::readHelperTile(out, format, inputPath);
        });
        return writeEncodedTile(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), format, tz, tx, ty,
                                outBuffer, outBufferSize);
    }

    std::optional<raster::PhotoGeoreference> photoToTile(const fs::path &file)
    {
        if (raster::isLibddbArtefact(file) || entryHandler(fingerprint(file)).tilesWithoutReprojection())
            return std::nullopt;
        std::string problem;
        auto photo = photoGeoreferenceOf(file, problem);
        if (!photo)
            throw FSException("Cannot geoproject " + io::toUtf8(file) + ", " + problem);
        return photo;
    }

} // namespace ddb
