/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TILERHELPER_H
#define TILERHELPER_H

#include "gdal_inc.h"

#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

#include "fs.h"
#include "geo.h"
#include "raster_tiler.h"
#include "tiler.h"
#include "output_format.h"
#include "thumbs.h"

namespace ddb
{

    // Where the tiles of a file come from, and where they are kept
    struct TileSource
    {
        fs::path path{};           // a georaster, a geoimage, a COPC point cloud or a URL
        int tileSize = 256;
        // DDBTile's flag, inverted with respect to its name: true when ty is an XYZ row (row 0 in the
        // north), false when it is a TMS row (row 0 in the south)
        bool tms = false;
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
        // The rows of x/y and of the tiles written (<z>/<x>/<y>.png) count from the south (TMS)
        // instead of the north (XYZ)
        bool tms = false;
        OutputFormat format = OutputFormat::Text;
        std::string zRange = "auto";   // "N" or "min-max"
        std::string x = "auto";        // with y: a single tile of each zoom level
        std::string y = "auto";
        // Replace tiles that exist. Without it the run writes nothing when one of its tiles is
        // there (OutputExistsException): every tile is checked before the first is written.
        bool overwrite = false;
    };

    class TilerHelper
    {
        // Parses a string (either "N" or "min-max") and returns
        // two ints (min,max)
        static BoundingBox<int> parseZRange(const std::string &zRange);

        // Where to store local cache tiles: a folder per file version, tile size and version of
        // ddb-tilers (which renders them), so a new renderer never serves the tiles of an old one
        static fs::path getCacheFolderName(const fs::path &tileablePath,
                                           time_t modifiedTime, int tileSize);

        // Runs render with a tiler for the source: one prepared earlier and kept in
        // RasterCache when available, otherwise a new one (a PhotoTiler for a photo)
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

        // A tiler of the raster or photo at `tileablePath` (a URL is downloaded first, toLocalFile):
        //  - a file libddb wrote (the COG of a build) is tiled as it is;
        //  - a photo that can be placed on the ground is drawn from itself tile by tile, in the raster
        //    helper (PhotoTiler, photoToTile);
        //  - any other raster is converted by the raster helper into a COG in a request folder the tiler
        //    keeps while it lives (the tiler reads EPSG:3857 COGs that libddb wrote, raster_artefacts.h).
        // Nothing of it is left once the tiler goes.
        static std::unique_ptr<Tiler> tilerFor(const fs::path &tileablePath, const std::string &tilesFolder,
                                               int tileSize = 256, bool tms = false,
                                               const std::string &tileablePathHash = "");

        // Cleans up the cache with the default ages (cache_cleanup.h): tiles, downloads, DSM tiles and what a
        // crash left. A tile request (getFromUserCache) and a new tiler of a user file (tilerFor) run it on
        // their way, one in a thousand
        static void cleanupUserCache();
    };

    // Test seam: while it lives, the tile requests and conversions of the process clean up the cache on
    // their way one in `chance` (0: never) instead of one in a thousand, so that a test that ages what is in
    // the cache decides when the clean-up runs (TilerHelper::cleanupUserCache): one that ran at random
    // first would remove what the test aged before the request touched it. The previous chance is back
    // when it ends.
    class ScopedCacheCleanupChanceForTesting
    {
    public:
        explicit ScopedCacheCleanupChanceForTesting(unsigned chance);
        ~ScopedCacheCleanupChanceForTesting();
        ScopedCacheCleanupChanceForTesting(const ScopedCacheCleanupChanceForTesting &) = delete;
        ScopedCacheCleanupChanceForTesting &operator=(const ScopedCacheCleanupChanceForTesting &) = delete;

    private:
        unsigned previous_;
    };

} // namespace ddb

#endif // TILERHELPER_H
