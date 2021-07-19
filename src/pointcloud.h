/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef POINTCLOUD_H
#define POINTCLOUD_H

#include <string>
#include <vector>
#include "ddb_export.h"
#include "json.h"
#include "basicgeometry.h"

namespace ddb{

struct PointCloudInfo{
    size_t pointCount;
    std::string wktProjection;
    std::vector<std::string> dimensions;
    std::vector<double> bounds;
    BasicPointGeometry centroid;
    BasicPolygonGeometry polyBounds;

    json toJSON();
};

DDB_DLL bool getPointCloudInfo(const std::string &filename, PointCloudInfo &info, int polyboundsSrs = 4326);
DDB_DLL bool getEptInfo(const std::string &eptJson, PointCloudInfo &info, int polyboundsSrs = 4326, int *span = nullptr);
DDB_DLL void buildEpt(const std::vector<std::string> &filenames, const std::string &outdir);

}

#endif // POINTCLOUD_H
