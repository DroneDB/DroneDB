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

    /// Octree node of a COPC file (depth and cell position)
    struct CopcNodeKey
    {
        int32_t d = 0, x = 0, y = 0, z = 0;
    };

    /// A node of the hierarchy that holds points: where its LAZ chunk lies in the file
    struct CopcNode
    {
        CopcNodeKey key;
        uint64_t offset = 0;
        int32_t byteSize = 0;
        int32_t pointCount = 0;
    };

    /// What decoding a COPC file's points needs, read from its header, COPC info VLR and
    /// hierarchy (no point data)
    struct CopcIndex
    {
        double spacing = 0;                     // point spacing of the root node
        double centerX = 0, centerY = 0, centerZ = 0, halfSize = 0; // root cube
        int pointFormat = 0;                    // LAS point data format (6, 7 or 8)
        int pointSize = 0;                      // bytes per point record
        double scale[3] = {1, 1, 1};
        double offset[3] = {0, 0, 0};
        std::vector<CopcNode> nodes;            // every node with points

        /// Point counts by depth
        DDB_DLL CopcHierarchy hierarchy() const;

        /// Whether the point records carry Red/Green/Blue (formats 7 and 8)
        bool hasColor() const { return pointFormat == 7 || pointFormat == 8; }

        /// Cell of key in the cloud's coordinates (PDAL's copc::Key::bounds)
        DDB_DLL void nodeBounds(const CopcNodeKey &key, double &minX, double &minY, double &maxX,
                                double &maxY) const;
    };

    /// Reads the LAS header, COPC info VLR and every hierarchy page of a .copc.laz (local
    /// path or URL). Throws AppException when the file is not a valid COPC.
    DDB_DLL CopcIndex readCopcIndex(const std::string &copcPath);

    /// readCopcIndex(copcPath).hierarchy()
    DDB_DLL CopcHierarchy readCopcHierarchy(const std::string &copcPath);

    /// Decompresses the points of one node: coordinates in the cloud CRS and colours
    /// (zeros when the format has none). Throws AppException when the chunk cannot be read.
    DDB_DLL void decodeCopcNode(const std::string &copcPath, const CopcIndex &index, const CopcNode &node,
                                PointBatch &points);

    /// readers.copc's depth limit for its "resolution" option: nodes of depth below it
    /// are read
    DDB_DLL int copcDepthEnd(double spacing, double resolution);

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

    /// readers.copc options for the points of a 2D window (cloud CRS, bounds included) down to
    /// the depth for resolution; the bounds text keeps every bit of the doubles
    DDB_DLL pdal::Options copcWindowOptions(const std::string &copcPath, double minX, double minY, double maxX,
                                            double maxY, double resolution);

    /// The points readers.copc returns for that window and resolution, in batches (the
    /// reference CopcNodeReader matches)
    DDB_DLL void readCopcWindow(const std::string &copcPath, double minX, double minY, double maxX, double maxY,
                                double resolution, const std::function<void(PointBatch &)> &sink);

    /// Runs the (streamable) PDAL pipeline that ends at tail without holding its points:
    /// they go to sink in batches of up to chunkSize. Batches carry the Red/Green/Blue
    /// dimensions when the pipeline has them, zeros otherwise.
    DDB_DLL void forEachPointBatch(pdal::Stage &tail, const std::function<void(PointBatch &)> &sink,
                                   size_t chunkSize = 16384);
}

#endif // COPC_UTILS_H
