/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef COG_H
#define COG_H

#include <string>

#include "basicgeometry.h"
#include "build_budget.h"
#include "json_fwd.h"

namespace ddb {

// Builds the cloud optimized GeoTIFF `outputCog` from a raster. The warp and the compression use
// the threads of one build: DDB_BUILD_THREADS, lowered to options.threads.
void buildCog(const std::string& input, const std::string& outputCog, const BuildOptions& options = {});
void generateCogStats(const std::string& cogPath);

}

#endif  // COG_H
