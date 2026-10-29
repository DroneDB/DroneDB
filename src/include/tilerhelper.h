/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TILERHELPER_H
#define TILERHELPER_H

#include "gdal_inc.h"

#include <functional>
#include <optional>
#include <sstream>
#include <string>

#include "fs.h"
#include "geo.h"
#include "gdaltiler.h"
#include "output_format.h"
#include "thumbs.h"

namespace ddb
{

    // Where the tiles of a file come from, and where they are kept
    struct TileSource
    {
        fs::path path{};           // a georaster, a geoimage, a COPC point cloud or a URL
        int tileSize = 256;
        bool tms = false;          // tile rows count from the bottom (TMS) instead of the top (XYZ)
        bool forceRecreate = false;
        fs::path outputFolder{};   // tile files go here (empty: only a requested buffer is filled)
        std::string pathHash{};    // known hash of a remote file: names its local copy ("" when unknown)
    };

    // One tile of a source
    struct TileRequest
    {
        TileSource source{};
        int tz = 0, tx = 0, ty = 0;
        std::string format = "png";                // "png", "jpeg" or "webp"
        std::optional<ThumbVisParams> visParams{}; // visualization of rasters (point clouds render as usual)
        uint8_t **outBuffer = nullptr;             // the encoded tile goes here instead of a file
        int *outBufferSize = nullptr;
    };

    // What `ddb tile` writes: the tiles of one source, as text or a JSON array
    struct TilerRun
    {
        int tileSize = 256;
        bool tms = false;
        OutputFormat format = OutputFormat::Text;
        std::string zRange = "auto";   // "N" or "min-max"
        std::string x = "auto";        // with y: a single tile of each zoom level
        std::string y = "auto";
    };

    class TilerHelper
    {
        // Parses a string (either "N" or "min-max") and returns
        // two ints (min,max)
        static BoundingBox<int> parseZRange(const std::string &zRange);

        // Where to store local cache tiles
        static fs::path getCacheFolderName(const fs::path &tileablePath,
                                           time_t modifiedTime, int tileSize);

        // Runs render with a tiler for the source: one prepared earlier and kept in
        // RasterCache when available, otherwise a new one (geoprojecting the file first
        // when it is not a georaster)
        static std::string withTiler(const TileSource &source,
                                     const std::function<std::string(Tiler &)> &render);

        // Local copy of tileablePath: the path itself, or for a URL the file downloaded
        // into the user cache (not an artefact: it is a file of somebody else)
        static fs::path toLocalFile(const fs::path &tileablePath, const std::string &tileablePathHash);

    public:
        // Writes the tiles of input under output, one path per line (or one JSON string per
        // member of an array) to os
        static void runTiler(const fs::path &input, const fs::path &output, std::ostream &os,
                             const TilerRun &run = {});

        // Get a single tile from user cache: request.source.outputFolder, format, visParams and
        // the buffer are not used (the cache names its own folder and keeps PNG files)
        static fs::path getFromUserCache(const TileRequest &request);

        // Get a single tile: its path, or the encoded bytes in the request's buffer
        static fs::path getTile(const TileRequest &request);

        // Prepare a tileable file for tiling: the raster the tiler opens. The tiler reads only
        // files that libddb wrote (raster_artefacts.h), so
        //  - a file that is one already (the COG of a build) is returned as it is;
        //  - a georaster is converted by the raster helper into a COG of the artefacts folder of
        //    the cache (without a limit on its size); the same state of the file is converted once;
        //  - a geoimage that can be tiled is geoprojected into the same folder;
        //  - a URL is downloaded first (toLocalFile) and the download converted the same way.
        // forceRecreate makes the conversion again.
        static fs::path toGeoTIFF(const fs::path &tileablePath, bool forceRecreate = false,
                                  const std::string &tileablePathHash = "");

        // Removes what the tiles left in the cache for five days: tiles, downloads and artefacts
        static void cleanupUserCache();
    };

} // namespace ddb

#endif // TILERHELPER_H
