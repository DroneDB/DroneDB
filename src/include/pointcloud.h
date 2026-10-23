/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef POINTCLOUD_H
#define POINTCLOUD_H

#include <string>
#include <vector>
#include "build_layout.h"
#include "ddb_export.h"
#include "json_fwd.h"
#include "basicgeometry.h"

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

    DDB_DLL bool getPointCloudInfo(const std::string &filename, PointCloudInfo &info, int polyboundsSrs = 4326);

    // Reads metadata from a built COPC (.copc.laz) file. `span` returns an estimated
    // root-node span (in points along the side of the root cube) used for tile-zoom heuristics,
    // analogous to the EPT span field.
    DDB_DLL bool getCopcInfo(const std::string &copcPath, PointCloudInfo &info, int polyboundsSrs = 4326, int *span = nullptr);

    // Builds a Cloud Optimized Point Cloud (COPC) file from one or more input
    // point cloud files. Output is a single file named `cloud.copc.laz` placed inside `outdir`.
    DDB_DLL void buildCopc(const std::vector<std::string> &filenames, const std::string &outdir);

    DDB_DLL void translateToLas(const std::string &input, const std::string &outputLas);

    // The COPC artifact names (CopcBuildSubfolder, CopcFileName) are in build_layout.h.

    // Returns true if the path looks like a COPC file (i.e. ends with .copc.laz).
    DDB_DLL bool isCopcPath(const std::string &filename);

}

#endif // POINTCLOUD_H
