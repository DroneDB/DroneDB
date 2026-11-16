/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef POINTCLOUD_H
#define POINTCLOUD_H

#include <string>
#include <vector>
#include "build_budget.h"
#include "build_layout.h"
#include "fs.h"
#include "json_fwd.h"
#include "basicgeometry.h"
#include "pointcloud_info.h"

namespace ddb
{


    /// The properties of a point cloud entry: {"pointCount", "projection", "dimensions"}
    void to_json(json &j, const PointCloudInfo &info);

    // The info of the point cloud of a user (LAS, LAZ, E57, XYZ, PTS, read by ddb-tilers in the raster
    // helper; a PLY point cloud from its header), with its footprint in EPSG:4326. False for a file
    // that is not a point cloud of those formats.
    // @throws the exceptions of IRasterHelper (a GDALException for a file that cannot be read)
    bool getPointCloudInfo(const std::string &filename, PointCloudInfo &info);

    // The info of a built COPC (.copc.laz), read by ddb-tilers in this process (the file is the
    // output of a build), with its footprint in EPSG:footprintSrs. False when it cannot be read.
    bool getCopcInfo(const std::string &copcPath, PointCloudInfo &info, int footprintSrs = 4326);

    // The info of a point cloud as ddb-tilers reads it (ddbtPointCloudInfo), with the footprint of its
    // bounds in EPSG:4326 when it has a CRS: what the raster helper answers for a file of a user
    // (raster::op::kPointCloudInfo). Text files (XYZ, PTS) have no bounds, as the bounds of a text
    // file were never kept. A file of a user: only in the raster helper (std::logic_error elsewhere).
    // @throws GDALException for a file that cannot be read
    PointCloudInfo readPointCloudInfo(const fs::path &path);

    // Builds a Cloud Optimized Point Cloud (COPC) file from one or more input point cloud files
    // (LAS, LAZ, E57, PTS, XYZ, a PLY point cloud), merged into one. Output is a single file named
    // `cloud.copc.laz` placed inside `outdir`. ddb-tilers builds it in the raster helper
    // (IRasterHelper::buildCopc), on the threads of one build (DDB_BUILD_THREADS, lowered to
    // options.threads) and within the memory of DDBT_COPC_BUILD_MEM_MB (else 4 GiB, at most half
    // the memory of the machine). The progress goes to options with the phases of
    // copcPhaseOfFraction.
    // The build runs in a private folder made in `outdir` (raster::makePrivateFolder), which is
    // removed with what is left in it; the COPC is moved to `outdir`/cloud.copc.laz once it is
    // whole (raster::publishArtefact), and nothing else in `outdir` is touched. Before anything
    // is read: a cloud.copc.laz that is one of the inputs is refused (InvalidArgsException), and
    // one that exists unless overwrite (OutputExistsException; true for the build handler).
    // @throws FSException for an input that does not exist; InvalidArgsException for one that is
    // not a point cloud, and for a cloud of fewer than 2 points or of no extent in X or Y;
    // PDALException for a cloud that cannot be read; RetryableBuildException when the device is
    // full; the other exceptions of IRasterHelper
    void buildCopc(const std::vector<std::string> &filenames, const std::string &outdir,
                   const BuildOptions &options = {}, bool overwrite = true);

    // The phase of a COPC build at a fraction of its work, as ddb-tilers reports them: "planning"
    // (the inputs are opened and the octree planned) below 0.05, "reading" (the points are read
    // and sorted into the octree) below 0.4, "building" (the nodes are thinned, encoded and
    // written) below 1, "writing" (the end of the file) at 1
    const char *copcPhaseOfFraction(double fraction);

    // The COPC artifact names (CopcBuildSubfolder, CopcFileName) are in build_layout.h.

    // Returns true if the path looks like a COPC file (i.e. ends with .copc.laz).
    bool isCopcPath(const std::string &filename);

}

#endif // POINTCLOUD_H
