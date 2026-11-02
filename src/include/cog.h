/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef COG_H
#define COG_H

#include <string>

#include "basicgeometry.h"
#include "build_budget.h"
#include "fs.h"
#include "json_fwd.h"

namespace ddb {

// Builds the cloud optimized GeoTIFF `outputCog` from a raster (a file of a user: the raster
// helper reads it, raster_helper.h), always by decoding and encoding it again, also when it is a
// COG already: a raster is warped to the tile grid of Web Mercator, while an optimized COG
// (isOptimizedCog) is encoded again at its own grid, with the same size, geotransform and bands,
// lossless when it was and JPEG of its own quality when it was JPEG, and overviews computed again.
// The warp and the compression use the threads of one build: DDB_BUILD_THREADS,
// lowered to options.threads, taken from the CPU pool of this process. Writes the statistics
// next to it (generateCogStats). The helper writes both into a private folder made beside
// `outputCog` (raster::makePrivateFolder), never into the folder of the output: that folder is
// the caller's and holds other files (and on Windows the confined helper writes only where the
// host gave a low integrity label). They are moved to their names once the COG opens as a raster
// (raster::publishArtefact). Throws what IRasterHelper::buildCog throws (a full device is a
// RetryableBuildException, a process that prints nothing for the idle time one with the reason
// IdleTimeout).
void buildCog(const std::string& input, const std::string& outputCog, const BuildOptions& options = {});
// The same build, written as `produced` in a private folder the caller made for it
// (raster::makePrivateFolder) and left there for the caller to publish, with its statistics
// (statsSidecarOf) next to it. Throws GDALException when what the helper wrote does not open as
// a raster.
void buildCogInPrivateFolder(const std::string& input, const fs::path& produced,
                             const BuildOptions& options = {});
void generateCogStats(const std::string& cogPath);

}

#endif  // COG_H
