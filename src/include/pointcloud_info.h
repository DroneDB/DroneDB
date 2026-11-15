/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef POINTCLOUD_INFO_H
#define POINTCLOUD_INFO_H

#include <cstddef>
#include <string>
#include <vector>

#include "basicgeometry.h"

namespace ddb
{
    // What an entry of a point cloud keeps of it: its count, CRS and dimensions (its properties),
    // and its bounds and footprint
    struct PointCloudInfo
    {
        size_t pointCount = 0;
        std::string wktProjection;
        std::vector<std::string> dimensions;
        std::vector<double> bounds;
        BasicPointGeometry centroid;
        BasicPolygonGeometry polyBounds;
        /// Why polyBounds and centroid are empty although the file has a coordinate reference
        /// system and bounds (a CRS that has no transformation to EPSG:4326, for one); "" otherwise.
        /// The native bounds are kept: a build needs no footprint. Not part of the properties JSON.
        std::string footprintWarning;
    };
}

#endif // POINTCLOUD_INFO_H
