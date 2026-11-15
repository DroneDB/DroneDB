/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CLOUD_SAMPLE_H
#define CLOUD_SAMPLE_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

#include "fs.h"

namespace ddb
{
    // A sample of a point cloud: the points a training export of Gaussian splats starts from. The raster
    // helper reads the cloud of a user and writes its sample to a file (sampleCloudFile,
    // writeCloudSample, operation sampleCloud); the host reads that file (readCloudSample) and never the
    // cloud.

    // The cell of the thinning grid starts at this size (meters) and grows by this factor while there
    // are too many points: the cube root of 2, so that the points of a surface (which fall as the
    // square of the cell) fall by about a third, not by 3/4, and the grid ends close to the limit
    constexpr double InitialCellSize = 0.001;
    constexpr double CellGrowth = 1.25;

    struct SampledPoint
    {
        double x, y, z;
        uint16_t r, g, b;
    };

    // Thins a stream of points on a 3D grid down to at most `limit`: the first point of each
    // cell stays, and the cell grows while there are more cells than the limit. One
    // pass, no knowledge of the cloud (its size, its extent) and memory of the limit.
    class GridSampler
    {
    public:
        explicit GridSampler(size_t limit) : limit_(limit) { points_.reserve(std::min<size_t>(limit, 1u << 20)); }

        void add(double x, double y, double z, uint16_t r, uint16_t g, uint16_t b)
        {
            ++seen_;
            const auto key = keyOf(x, y, z);
            if (!key || limit_ == 0)
                return;
            if (cells_.try_emplace(*key, static_cast<uint32_t>(points_.size())).second)
            {
                points_.push_back({x, y, z, r, g, b});
                if (points_.size() > limit_)
                    coarsen();
            }
        }

        size_t seen() const { return seen_; }
        const std::vector<SampledPoint> &points() const { return points_; }

    private:
        struct Key
        {
            int64_t x, y, z;
            bool operator==(const Key &o) const { return x == o.x && y == o.y && z == o.z; }
        };
        struct KeyHash
        {
            size_t operator()(const Key &k) const
            {
                size_t h = std::hash<int64_t>()(k.x);
                h = h * 0x9E3779B97F4A7C15ull + std::hash<int64_t>()(k.y);
                h = h * 0x9E3779B97F4A7C15ull + std::hash<int64_t>()(k.z);
                return h;
            }
        };

        // Nothing for a coordinate that is not finite or too far for the grid
        std::optional<Key> keyOf(double x, double y, double z) const
        {
            const double gx = std::floor(x / cell_), gy = std::floor(y / cell_), gz = std::floor(z / cell_);
            constexpr double Limit = 4e18;
            if (!(std::abs(gx) < Limit) || !(std::abs(gy) < Limit) || !(std::abs(gz) < Limit))
                return std::nullopt;
            return Key{static_cast<int64_t>(gx), static_cast<int64_t>(gy), static_cast<int64_t>(gz)};
        }

        // Grows the cell until the points that stay fit; the first of the points that share a
        // cell stays
        void coarsen()
        {
            while (points_.size() > limit_)
            {
                cell_ *= CellGrowth;
                cells_.clear();
                std::vector<SampledPoint> kept;
                kept.reserve(points_.size());
                for (const auto &p : points_)
                    if (cells_.try_emplace(*keyOf(p.x, p.y, p.z), static_cast<uint32_t>(kept.size())).second)
                        kept.push_back(p);
                points_ = std::move(kept);
            }
        }

        size_t limit_;
        double cell_ = InitialCellSize;
        size_t seen_ = 0;
        std::unordered_map<Key, uint32_t, KeyHash> cells_;
        std::vector<SampledPoint> points_;
    };


    // What a sample takes of a cloud: its points shifted by -shift (the offset of the local frame of
    // an ODM project for its georeferenced LAS, none for its filtered PLY), thinned to at most `limit`
    struct CloudSampleParams
    {
        std::array<double, 3> shift{0, 0, 0};
        size_t limit = 0;
    };

    // The sample of the cloud at `path` (LAS, LAZ, PLY, E57, XYZ, PTS, read by ddb-tilers in file
    // order), with the fraction of its points read reported to `progress`. A file of a user: only in
    // the raster helper (std::logic_error elsewhere).
    // @throws GDALException for a cloud that cannot be read; CanceledException
    std::vector<SampledPoint> sampleCloudFile(const fs::path &path, const CloudSampleParams &params,
                                              const std::function<void(double)> &progress);

    // Writes a sample to `out` and reads it back: "DDBCLOUDSAMPLE1\0", the count (uint64), then each
    // point as x, y, z (float64) and r, g, b (uint16), little-endian
    // @throws FSException
    void writeCloudSample(const fs::path &out, const std::vector<SampledPoint> &points);
    // @throws AppException for a file that is not a whole sample
    std::vector<SampledPoint> readCloudSample(const fs::path &in);
}

#endif // CLOUD_SAMPLE_H
