/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef COPC_UTILS_H
#define COPC_UTILS_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "parallel.h"
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
        double centerX = 0, centerY = 0, halfSize = 0; // root cube
        int pointFormat = 0;                    // LAS point data format (6, 7 or 8)
        int pointSize = 0;                      // bytes per point record
        double scale[3] = {1, 1, 1};
        double offset[3] = {0, 0, 0};
        std::vector<CopcNode> nodes;            // every node with points

        /// Point counts by depth
        CopcHierarchy hierarchy() const;

        /// Whether the point records carry Red/Green/Blue (formats 7 and 8)
        bool hasColor() const { return pointFormat == 7 || pointFormat == 8; }

        /// Cell of key in the cloud's coordinates (PDAL's copc::Key::bounds)
        void nodeBounds(const CopcNodeKey &key, double &minX, double &minY, double &maxX,
                        double &maxY) const;
    };

    /// Reads the LAS header, COPC info VLR and every hierarchy page of a .copc.laz (local
    /// path or URL). Throws AppException when the file is not a valid COPC.
    CopcIndex readCopcIndex(const std::string &copcPath);

    /// readCopcIndex(copcPath).hierarchy()
    CopcHierarchy readCopcHierarchy(const std::string &copcPath);

    /// Decompresses the points of one node: coordinates in the cloud CRS and colours
    /// (zeros when the format has none). Throws AppException when the chunk cannot be read.
    void decodeCopcNode(const std::string &copcPath, const CopcIndex &index, const CopcNode &node,
                        PointBatch &points);

    /// readers.copc's depth limit for its "resolution" option: nodes of depth below it
    /// are read
    int copcDepthEnd(double spacing, double resolution);

    /// Deepest octree depth whose points, with those of every shallower depth, fit in
    /// budget (0 when even the root exceeds it)
    int planCopcDepth(const std::vector<uint64_t> &pointsPerDepth, uint64_t budget);

    /// readers.copc "resolution" option that reads depths 0..depth of a cloud whose
    /// root spacing is spacing. PDAL reads the depths below ceil(log2(spacing / r)) + 1,
    /// so the value is taken between two levels, never on one.
    double copcResolutionForDepth(double spacing, int depth);

    /// CPU tokens for the decompression threads of a local COPC file: up to 4 threads, the
    /// calling one included (parallel::tryAcquireThreads). None for a remote file, which keeps
    /// PDAL's own thread count (network-bound). Keep them until the reader has run.
    parallel::CpuTokens copcReadThreads(const std::string &copcPath);

    /// readers.copc options shared by tiles and thumbnails: the file (relative paths
    /// made explicit for PDAL), no SRS processing (callers work with the WKT of
    /// getCopcInfo and pass bounds in the cloud CRS) and, for a local file, the
    /// decompression threads that `threads` (copcReadThreads) allows
    pdal::Options copcReaderOptions(const std::string &copcPath, const parallel::CpuTokens &threads);

    /// A 2D window of a COPC file (cloud CRS, bounds included) and the resolution to read it at
    struct CopcWindow
    {
        double minX = 0, minY = 0, maxX = 0, maxY = 0;
        double resolution = 0;
    };

    /// readers.copc options for the points of a window, down to the depth for its resolution;
    /// the bounds text keeps every bit of the doubles
    pdal::Options copcWindowOptions(const std::string &copcPath, const parallel::CpuTokens &threads,
                                    const CopcWindow &window);

    /// Runs the (streamable) PDAL pipeline that ends at tail without holding its points:
    /// they go to sink in batches of up to chunkSize. Batches carry the Red/Green/Blue
    /// dimensions when the pipeline has them, zeros otherwise.
    void forEachPointBatch(pdal::Stage &tail, const std::function<void(PointBatch &)> &sink,
                           size_t chunkSize = 16384);
}

#endif // COPC_UTILS_H
