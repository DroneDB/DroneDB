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

// True for a raster driver that decodes on threads of its own (JPEG 2000). It sizes them from
// GDAL_NUM_THREADS as the reading thread sees it, and uses every CPU when that is unset.
bool decodesWithOwnThreads(const std::string& driverName);

}

#endif  // COG_UTILS_H