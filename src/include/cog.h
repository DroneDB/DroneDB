/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef COG_H
#define COG_H

#include <functional>
#include <optional>
#include <string>

#include "basicgeometry.h"
#include "build_budget.h"
#include "fs.h"
#include "json_fwd.h"
#include "raster_protocol.h"

namespace ddb {

// Builds the cloud optimized GeoTIFF `outputCog` from a raster (a GeoTIFF or JPEG 2000 of a user:
// the raster helper reads it, raster_helper.h, and ddb-tilers builds it), always by decoding and
// encoding it again, also when it is a COG already: a raster is warped to the tile grid of Web
// Mercator, while an optimized COG (isOptimizedCog) is encoded again at its own grid, with the same
// size, geotransform and bands, lossless when it was and JPEG of its own quality when it was JPEG,
// and overviews computed again. The warp and the compression use the threads of one build:
// DDB_BUILD_THREADS, lowered to options.threads, taken from the CPU pool of this process. Writes
// the exact statistics of every band next to it (statsSidecarOf). The helper writes both into a private folder made beside
// `outputCog` (raster::makePrivateFolder), never into the folder of the output: that folder is
// the caller's and holds other files (and on Windows the confined helper writes only where the
// host gave a low integrity label). They are moved to their names once the COG opens as a raster
// (raster::publishArtefact). Throws what IRasterHelper::buildCog throws (a full device is a
// RetryableBuildException, a process that prints nothing for the idle time one with the reason
// IdleTimeout).
// An output that is the input is refused (InvalidArgsException). An output that exists is
// replaced when `overwrite` is true (the build handlers and the other internal callers), and
// otherwise refused before anything is built (OutputExistsException, io::assureReplaceable).
void buildCog(const std::string& input, const std::string& outputCog, const BuildOptions& options = {},
              bool overwrite = true);
// The same build, written as `produced` in a private folder the caller made for it
// (raster::makePrivateFolder) and left there for the caller to publish, with its statistics
// (statsSidecarOf) next to it. A photo (which has no georeferencing of its own) is placed on the
// ground by `photo` and warped from it in the same pass. Throws GDALException when what the
// helper wrote does not open as a raster.
void buildCogInPrivateFolder(const std::string& input, const fs::path& produced,
                             const BuildOptions& options = {},
                             const std::optional<raster::PhotoGeoreference>& photo = std::nullopt);

// The role of the COG of a GeoRaster in its build
inline constexpr const char* CogRole = "raster.cog";

class Database;

// What buildCogStatistics found or did
enum class CogStatisticsStatus
{
    Computed,  // the statistics were computed and written next to the COG
    Present,   // the COG had them already (a sidecar that is used)
    NotBuilt   // the entry has no complete build: nothing to compute
};

// DDBBuildRasterStatistics: the exact statistics of every band of the COG of the GeoRaster entry `entryPath`, kept
// next to it (statsSidecarOf) when it has none that is used: a COG built before libddb wrote them at build time
// gets them without being built again. One parallel pass over its pixels (RasterStatsProvider::getAll), on at most
// `maxThreads` threads (0: DDB_STATS_THREADS); a COG in which no band has a pixel with a value gets a sidecar
// without an entry, so it is not passed again. The build lock of the entry is held meanwhile.
// `progress`, when set, is told the fraction done. Can be canceled.
// @throws EntryNotFoundException (no such entry), InvalidArgsException (not a GeoRaster), BuildInProgressException
// (a build of the entry runs), GDALException (the COG cannot be read)
CogStatisticsStatus buildCogStatistics(Database* db, const std::string& entryPath, size_t maxThreads,
                                       const std::function<void(double)>& progress = {});

}

#endif  // COG_H
