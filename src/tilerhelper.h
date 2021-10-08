/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TILERHELPER_H
#define TILERHELPER_H

#include "gdal_inc.h"

#include <sstream>
#include <string>

#include "ddb_export.h"
#include "fs.h"
#include "geo.h"
#include "gdaltiler.h"

namespace ddb {


class TilerHelper {
    // Parses a string (either "N" or "min-max") and returns
    // two ints (min,max)
    static BoundingBox<int> parseZRange(const std::string &zRange);

    // Where to store local cache tiles
    static fs::path getCacheFolderName(const fs::path &tileablePath,
                                       time_t modifiedTime, int tileSize);

   public:
    DDB_DLL static void runTiler(const fs::path &input,
                                 const fs::path &output,
                                 int tileSize = 256,
                                 bool tms = false,
                                 std::ostream &os = std::cout,
                                 const std::string &format = "text",
                                 const std::string &zRange = "auto",
                                 const std::string &x = "auto",
                                 const std::string &y = "auto");

    // Get a single tile from user cache
    DDB_DLL static fs::path getFromUserCache(const fs::path &tileablePath,
                                             int tz, int tx, int ty,
                                             int tileSize, bool tms,
                                             bool forceRecreate,
                                             const std::string &tileablePathHash = "");

    // Get a single tile
    DDB_DLL static fs::path getTile(const fs::path &tileablePath,
                                int tz, int tx, int ty,
                                int tileSize, bool tms,
                                bool forceRecreate,
                                const fs::path &outputFolder,
                                uint8_t **outBuffer = nullptr, int *outBufferSize = nullptr,
                                const std::string &tileablePathHash = "");

    // Prepare a tileable file for tiling (if needed)
    // for example, geoimages that can be tiled are first geoprojected
    DDB_DLL static fs::path toGeoTIFF(const fs::path &tileablePath,
                                      int tileSize, bool forceRecreate,
                                      const fs::path &outputGeotiff = "",
                                      const std::string &tileablePathHash = "");

    DDB_DLL static void cleanupUserCache();
};

}  // namespace ddb

#endif  // TILERHELPER_H
