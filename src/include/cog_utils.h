/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef COG_UTILS_H
#define COG_UTILS_H

#include <string>

namespace ddb {

// Check if a GeoTIFF file is already an optimized Cloud Optimized GeoTIFF
// Returns true if the file is already optimized and doesn't need rebuild
bool isOptimizedCog(const std::string& inputPath);

}

#endif  // COG_UTILS_H