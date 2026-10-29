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
    // reads EXIF or other tags, which stay with the metadata helper. The host does not call them:
    // it asks the helper (raster_helper.h), which runs them in a process of its own.
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

        // Adds what a probe found to the entry: its properties, the projection, the geometries
        // and its warning
        void applyProbe(const ProbeResult &result, Entry &entry);

        // The thumbnail of a photo or a raster, in memory
        // @throws RasterOpException when the file cannot be opened; GDALException,
        // InvalidArgsException for a size that is not positive
        RasterImage renderThumbnail(const fs::path &path, const ThumbnailParams &params);

        // The same thumbnail, written to outFile as the thumbnails are (WebP)
        void thumbnail(const fs::path &path, const fs::path &outFile, const ThumbnailParams &params);

        // The long operations below run in a process of their own (`gdal-util --request`). The
        // COG build reports how far it is through `progress` (called from any thread, at most
        // once per percent), which may be empty.

        // Builds the Cloud Optimized GeoTIFF outFile (Web Mercator, tiled, with overviews) from a
        // raster with a projection, and the statistics next to it (<outFile>.stats.json). Always
        // decodes and encodes the raster, also when it is a COG already: the file a COG build
        // leaves is what GDAL wrote here, never a copy of a file of a user.
        // @throws RasterOpException when the file cannot be opened; GDALException when it has
        // no projection or GDAL fails; RetryableBuildException when the device is full
        void buildCogFile(const fs::path &path, const fs::path &outFile, const CogParams &params,
                          const ProgressCallback &progress);

        // Builds the vector outputs of a file in the folder outFolder, which exists and is
        // empty: vec/source.gpkg and, unless the file is a CAD drawing without a coordinate
        // system, the MVT tiles in mvt/ (a drawing without one gets vec/local.json instead).
        // A DWG is converted by the caller; `path` is the DXF then (VectorParams::source).
        // @throws RasterOpException when the file cannot be opened; AppException,
        // RetryableBuildException
        void buildVectorFiles(const fs::path &path, const fs::path &outFolder, const VectorParams &params);

        // Writes the GeoTIFF outFile of a photo placed on the ground by the corners of
        // GeoprojectParams (JPEG compressed, with an alpha band)
        // @throws RasterOpException when the file cannot be opened; GDALException,
        // InvalidArgsException for an outsize that is not a positive number
        void geoprojectImage(const fs::path &path, const fs::path &outFile, const GeoprojectParams &params);

    } // namespace raster
} // namespace ddb

#endif // RASTER_OPS_H
