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

namespace pdal
{
    class PointView;
}

namespace ddb
{

    struct PointCloudInfo
    {
        size_t pointCount;
        std::string wktProjection;
        std::vector<std::string> dimensions;
        std::vector<double> bounds;
        BasicPointGeometry centroid;
        BasicPolygonGeometry polyBounds;

        json toJSON();
    };

    struct PointColor
    {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a = 0;
    };

    DDB_DLL bool getPointCloudInfo(const std::string &filename, PointCloudInfo &info, int polyboundsSrs = 4326);
    DDB_DLL bool getEptInfo(const std::string &eptJson, PointCloudInfo &info, int polyboundsSrs = 4326, int *span = nullptr);
    DDB_DLL void buildEpt(const std::vector<std::string> &filenames, const std::string &outdir);
    DDB_DLL std::vector<PointColor> normalizeColors(const std::shared_ptr<pdal::PointView>& point_view);
    DDB_DLL std::vector<PointColor> generateZBasedColors(const std::shared_ptr<pdal::PointView>& point_view, double minZ, double maxZ);
    DDB_DLL void translateToLas(const std::string &input, const std::string &outputLas);

}

#endif // POINTCLOUD_H
