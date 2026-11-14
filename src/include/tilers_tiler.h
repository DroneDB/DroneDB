/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TILERS_TILER_H
#define TILERS_TILER_H

#include <string>

#include "tiler.h"

namespace ddb
{
    /// Renders the web-map tiles of a file with ddb-tilers: an EPSG:3857 COG (RasterTiler) or a
    /// COPC point cloud, local or at an http(s) URL (read through libddb's HTTP client: see
    /// tilers::useHttpClientForRemoteReads). The bounds and the zoom range are ddb-tilers'
    /// (ddbtTileInfo); every tile is rendered and encoded by ddb-tilers, which keeps the file open
    /// and caches decoded data and encoded tiles between calls, per version of the file, so a
    /// rebuilt file is never served the tiles of the one it replaced (ddbtReleaseSources closes
    /// it). A tile out of the bounds fails with "Out of bounds". An instance holds no state a tile
    /// changes: threads may share it.
    class TilersTiler : public Tiler
    {
    public:
        /// @throws the exceptions of tilers::check when ddb-tilers cannot read the file
        TilersTiler(const std::string &path, const std::string &tilesFolder, int size = 256, bool tmsRows = false);

        using Tiler::tile;

        std::string tile(int tz, int tx, int ty, ImageFormat format, uint8_t **outBuffer = nullptr,
                         int *outBufferSize = nullptr) override;
    };
} // namespace ddb

#endif // TILERS_TILER_H
