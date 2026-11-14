/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef RASTER_TILER_H
#define RASTER_TILER_H

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
    };
} // namespace ddb

#endif // RASTER_TILER_H
