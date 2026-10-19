/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef COPC_UTILS_H
#define COPC_UTILS_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ddb_export.h"
#include "point_rasterizer.h"

namespace pdal
{
    class Options;
    class Stage;
}

namespace ddb
{
    /// Point counts of a COPC octree by depth, from its hierarchy (no point data read)
    struct CopcHierarchy
    {
        double spacing = 0;                  // point spacing of the root node
        std::vector<uint64_t> pointsPerDepth; // index = depth (0 = root)

        uint64_t totalPoints() const;
    };

    /// Reads the COPC info VLR and every hierarchy page of a .copc.laz (local path or
    /// URL). Throws AppException when the file is not a valid COPC.
    DDB_DLL CopcHierarchy readCopcHierarchy(const std::string &copcPath);

    /// Deepest octree depth whose points, with those of every shallower depth, fit in
    /// budget (0 when even the root exceeds it)
    DDB_DLL int planCopcDepth(const std::vector<uint64_t> &pointsPerDepth, uint64_t budget);

    /// readers.copc "resolution" option that reads depths 0..depth of a cloud whose
    /// root spacing is spacing. PDAL reads the depths below ceil(log2(spacing / r)) + 1,
    /// so the value is taken between two levels, never on one.
    DDB_DLL double copcResolutionForDepth(double spacing, int depth);

    /// readers.copc options shared by tiles and thumbnails: the file (relative paths
    /// made explicit for PDAL), no SRS processing (callers work with the WKT of
    /// getCopcInfo and pass bounds in the cloud CRS) and up to 4 decompression threads
    DDB_DLL pdal::Options copcReaderOptions(const std::string &copcPath);

    /// Runs the (streamable) PDAL pipeline that ends at tail without holding its points:
    /// they go to sink in batches of up to chunkSize. Batches carry the Red/Green/Blue
    /// dimensions when the pipeline has them, zeros otherwise.
    DDB_DLL void forEachPointBatch(pdal::Stage &tail, const std::function<void(PointBatch &)> &sink,
                                   size_t chunkSize = 16384);
}

#endif // COPC_UTILS_H
