/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef RASTER_TILER_H
#define RASTER_TILER_H

#include <mutex>
#include <optional>
#include <string>

#include "tilers_tiler.h"

namespace ddb
{
    /// Renders web-map tiles of an EPSG:3857 COG that libddb wrote (an artefact,
    /// raster_artefacts.h: the COG of a build, or the COG TilerHelper makes from a raster or a
    /// photo of a user) with ddb-tilers (TilersTiler), with the visualization parameters of a
    /// raster. A tile that does not exist at its zoom is out of the raster: transparent, or "Out of
    /// bounds" with visualization parameters.
    class RasterTiler : public TilersTiler
    {
    public:
        /// @throws GDALException when the file is not an artefact or ddb-tilers cannot read it
        RasterTiler(const std::string &cogPath, const std::string &tilesFolder, int size = 256, bool tmsRows = false);

        using TilersTiler::tile;

        std::string tile(int tz, int tx, int ty, ImageFormat format, uint8_t **outBuffer = nullptr,
                         int *outBufferSize = nullptr) override;
        std::string tile(int tz, int tx, int ty, const ThumbVisParams &visParams, ImageFormat format,
                         uint8_t **outBuffer = nullptr, int *outBufferSize = nullptr) override;

        /// The box of the pixels of the COG that are not transparent (tilers::Raster::opaqueWindow), read
        /// on the first call. The bounds of the COG are those of the tiles of its zoom level that hold its
        /// data: a raster smaller than a tile has the bounds of the whole tile, which hold thousands of
        /// transparent tiles of the zoom levels above it.
        std::optional<BoundingBox<Projected2D>> contentBounds() const override;

    private:
        mutable std::once_flag contentOnce;
        mutable std::optional<BoundingBox<Projected2D>> content;
    };
} // namespace ddb

#endif // RASTER_TILER_H
