/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef POINTCLOUD_H
#define POINTCLOUD_H

#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "build_budget.h"
#include "build_layout.h"
#include "fs.h"
#include "json_fwd.h"
#include "basicgeometry.h"
#include "pointcloud_crs.h"
#include "pointcloud_info.h"

namespace ddb
{


    /// The properties of a point cloud entry: {"pointCount", "projection" (the CRS the file states,
    /// WKT), "dimensions", "crsStatus" (toString(PointCloudCrsStatus))}
    void to_json(json &j, const PointCloudInfo &info);

    // What a COPC build did with the coordinate systems of its clouds. The build of an entry writes
    // it beside the COPC (CopcBuildRecordFileName) as {"crsPolicy", "sourceCrs", "copcCrs",
    // "transformed"}: the version of PointCloudCrsPolicy it was built under, the WKT of the CRS of the
    // (first) cloud and of the COPC ("" without one), and whether the points were transformed.
    struct CopcBuildRecord
    {
        int crsPolicy = PointCloudCrsPolicy::kVersion;
        std::string sourceCrs;
        std::string copcCrs;
        bool transformed = false;
    };

    void to_json(json &j, const CopcBuildRecord &record);
    // @throws FSException when the file cannot be written
    void writeCopcBuildRecord(const fs::path &file, const CopcBuildRecord &record);
    // Nothing when the file does not exist or is not a build record
    std::optional<CopcBuildRecord> readCopcBuildRecord(const fs::path &file);

    // The info of the point cloud of a user (LAS, LAZ, E57, XYZ, PTS, read by ddb-tilers in the raster
    // helper; a PLY point cloud from its header), with its footprint in EPSG:4326. False for a file
    // that is not a point cloud of those formats.
    // @throws the exceptions of IRasterHelper (a GDALException for a file that cannot be read)
    bool getPointCloudInfo(const std::string &filename, PointCloudInfo &info);

    // Runs `read`, a read of a user's point cloud in the raster helper. A cloud the helper cannot read
    // is a PDALException (the add error code "PDAL"), as PDAL's readers reported it when libddb read
    // the clouds itself; `context` comes before the reason.
    void readPointCloud(const std::string &context, const std::function<void()> &read);

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
    // `cloud.copc.laz` placed inside `outdir`. The COPC of a geographic or geocentric cloud is built
    // in the projected CRS PointCloudCrsPolicy chooses for it (for clouds merged into one, the
    // choice for the first that has one), and the build fails when the COPC is not in that CRS or
    // its octree still does not split the cloud. The COPC of a cloud whose CRS does not fit its
    // coordinates (an implausible one) states no CRS, its points as they are. Returns what was done with the CRSs, which the
    // build of an entry records (writeCopcBuildRecord). ddb-tilers builds it in the raster helper
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
    // PDALException for a cloud that cannot be read; AppException for a COPC that is not in the
    // CRS asked for or whose octree is still degenerate; RetryableBuildException when the device is
    // full; the other exceptions of IRasterHelper
    // `known` (in the order of filenames, nothing for a cloud the helper reads) is what the caller
    // already knows of the CRS of a cloud: its crsStatus, wktProjection and targetCrs
    // (chooseCopcTarget), as the index of a dataset has them.
    CopcBuildRecord buildCopc(const std::vector<std::string> &filenames, const std::string &outdir,
                              const BuildOptions &options = {}, bool overwrite = true,
                              const std::vector<std::optional<PointCloudInfo>> &known = {});

    // Sets info.targetCrs, the CRS the COPC of the cloud is built in, from its crsStatus,
    // wktProjection and footprint in WGS 84 (polyBounds): PointCloudCrsPolicy::target for a
    // geographic or geocentric cloud with a footprint, "" otherwise (also when PROJ cannot make
    // the target, which is logged with the name of the cloud). The one choice of the target for a
    // cloud the helper reads and for an entry of the index.
    void chooseCopcTarget(PointCloudInfo &info, const std::string &cloud);

    // The crsStatus an entry's properties record; nothing for an entry indexed before it was
    // recorded, or with a value that is not a status
    std::optional<PointCloudCrsStatus> indexedCrsStatus(const json &properties);

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
