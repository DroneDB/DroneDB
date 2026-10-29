/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef RASTER_OPS_H
#define RASTER_OPS_H

#include <exception>

#include "entry.h"
#include "exceptions.h"
#include "fs.h"
#include "raster_protocol.h"
#include "tile_encoder.h"

namespace ddb
{
    // The operations that read a file a user supplied with GDAL: what the raster helper
    // (gdal-util) runs for a request of apps/gdal-util/PROTOCOL.md. Each takes the file and its
    // params and returns plain data (raster_protocol.h) or writes the file it is given; none
    // reads EXIF or other tags, which stay with the metadata helper. Until the host sends them to
    // the helper, the entry handlers call them in the process.
    namespace raster
    {
        // A file that cannot be read as what the operation needs: GDAL opens none of the drivers
        // its name allows, or the open policy refuses it. Not a failure of the operation itself.
        class RasterOpException : public GDALException
        {
          public:
            RasterOpException(ErrorCode code, const std::string &message) : GDALException(message), code_(code) {}
            ErrorCode code() const noexcept { return code_; }

          private:
            ErrorCode code_;
        };

        // The error code a failure is reported with: that of a RasterOpException, no space for a
        // full disk, a limit for an allocation that failed, a defect otherwise (and for the
        // failures of GDAL that are the file's, unreadable)
        ErrorCode classify(const std::exception &failure);

        // What GDAL finds in a raster or a vector file, as the entry of the file needs it. A
        // raster with a projection is a GeoRaster, whose size, geotransform, bands and WGS84
        // footprint are returned; any other raster only says it has none.
        // @throws RasterOpException when the file cannot be opened; GDALException when a
        // coordinate system cannot be read
        ProbeResult probe(const fs::path &path, const ProbeParams &params);

        // Adds what a probe found to the entry: its properties, the projection and the geometries
        void applyProbe(const ProbeResult &result, Entry &entry);

        // The thumbnail of a photo or a raster, in memory
        // @throws RasterOpException when the file cannot be opened; GDALException,
        // InvalidArgsException for a size that is not positive
        RasterImage renderThumbnail(const fs::path &path, const ThumbnailParams &params);

        // The same thumbnail, written to outFile as the thumbnails are (WebP)
        void thumbnail(const fs::path &path, const fs::path &outFile, const ThumbnailParams &params);

    } // namespace raster
} // namespace ddb

#endif // RASTER_OPS_H
