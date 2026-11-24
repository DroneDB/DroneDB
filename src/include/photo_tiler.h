/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef PHOTO_TILER_H
#define PHOTO_TILER_H

#include <optional>
#include <string>

#include "fs.h"
#include "raster_protocol.h"
#include "tiler.h"

namespace ddb
{
    /// The web map tiles of a photo (a geoimage, which has no georeferencing of its own), placed on the
    /// ground by the corners of its EXIF footprint. The host never reads the photo: each tile that holds
    /// some of the footprint is drawn from the photo by the raster helper for that request (the photoTile
    /// operation, raster::renderPhotoTile) and nothing of it is kept; a tile that holds none of it is the
    /// transparent tile, made here without a request. The bounds and the zoom levels are those of the COG
    /// gdalwarp would make of the photo (its suggested resolution, the first zoom level at least as fine,
    /// the extent snapped to the 256 px tiles of that level), as the COG builds of a photo make it, so the
    /// tiles of a photo are where they always were. Visualization parameters do not apply to a photo: its
    /// tiles are the plain ones. Holds no state a tile changes: threads may share it.
    class PhotoTiler : public Tiler
    {
    public:
        /// @throws FSException when the photo does not exist; GDALException for a tile size that is no power of 2
        PhotoTiler(const fs::path &photo, const std::string &tilesFolder, int size, bool tmsRows,
                   const raster::PhotoGeoreference &georeference);

        using Tiler::tile;

        std::string tile(int tz, int tx, int ty, ImageFormat format, uint8_t **outBuffer = nullptr,
                         int *outBufferSize = nullptr) override;

        /// The box of the footprint
        std::optional<BoundingBox<Projected2D>> contentBounds() const override;

    private:
        raster::PhotoGeoreference georeference_;
    };

    /// The georeference by which a file is tiled as a photo: none for a file libddb wrote (the COG of a
    /// build) or one whose entry handler tiles it without reprojection (a georaster)
    /// @throws FSException for a photo that cannot be placed on the ground (no footprint in its EXIF)
    std::optional<raster::PhotoGeoreference> photoToTile(const fs::path &file);

} // namespace ddb

#endif // PHOTO_TILER_H
