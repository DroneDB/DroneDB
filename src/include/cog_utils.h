/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef COG_UTILS_H
#define COG_UTILS_H

#include <string>

#include <gdal.h>

namespace ddb {

// Check if a GeoTIFF file is already an optimized Cloud Optimized GeoTIFF: Web Mercator, square
// tiles of 256 or 512 pixels, overviews, read by the GTiff or COG driver
bool isOptimizedCog(const std::string& inputPath);

// The same check on a dataset that is open already
bool isOptimizedCogDataset(GDALDatasetH hDataset);

}

#endif  // COG_UTILS_H