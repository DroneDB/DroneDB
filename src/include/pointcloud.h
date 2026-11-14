/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef POINTCLOUD_H
#define POINTCLOUD_H

#include <string>
#include <vector>
#include "build_budget.h"
#include "build_layout.h"
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
        /// Why polyBounds and centroid are empty although the file has a coordinate reference
        /// system and bounds (a CRS that has no transformation to EPSG:4326, for one); "" otherwise.
        /// The native bounds are kept: a build needs no footprint. Not part of the properties JSON.
        std::string footprintWarning;

    };

    /// The properties of a point cloud entry: {"pointCount", "projection", "dimensions"}
    void to_json(json &j, const PointCloudInfo &info);

    bool getPointCloudInfo(const std::string &filename, PointCloudInfo &info, int polyboundsSrs = 4326);

    // Reads metadata from a built COPC (.copc.laz) file. `span` returns an estimated
    // root-node span (in points along the side of the root cube) used for tile-zoom heuristics,
    // analogous to the EPT span field.
    bool getCopcInfo(const std::string &copcPath, PointCloudInfo &info, int polyboundsSrs = 4326, int *span = nullptr);

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
