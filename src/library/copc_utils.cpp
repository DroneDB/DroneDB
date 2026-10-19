/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "copc_utils.h"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <thread>

#include <pdal/Options.hpp>
#include <pdal/PointTable.hpp>
#include <pdal/PointRef.hpp>
#include <pdal/Stage.hpp>

#include "exceptions.h"
#include "gdal_inc.h"
#include "utils.h"

namespace ddb
{
    namespace
    {
        constexpr size_t LasHeaderSize = 375;  // LAS 1.4
        constexpr size_t VlrHeaderSize = 54;
        constexpr size_t HierarchyEntrySize = 32;

        template <typename T>
        T readLE(const uint8_t *p)
        {
            T v;
            std::memcpy(&v, p, sizeof(T)); // COPC is little-endian, as are our targets
            return v;
        }

        class VsiFile
        {
            VSILFILE *f_;
            std::string path_;

        public:
            explicit VsiFile(const std::string &path) : path_(path)
            {
                f_ = VSIFOpenL(path.c_str(), "rb");
                if (f_ == nullptr)
                    throw AppException("Cannot open " + path);
            }
            ~VsiFile() { VSIFCloseL(f_); }
            VsiFile(const VsiFile &) = delete;
            VsiFile &operator=(const VsiFile &) = delete;

            std::vector<uint8_t> read(uint64_t offset, size_t size)
            {
                std::vector<uint8_t> buf(size);
                if (VSIFSeekL(f_, offset, SEEK_SET) != 0 || VSIFReadL(buf.data(), 1, size, f_) != size)
                    throw AppException("Cannot read " + std::to_string(size) + " bytes at " +
                                       std::to_string(offset) + " of " + path_);
                return buf;
            }
        };

        // A table that hands each chunk of streamed points to a sink before PDAL reuses it
        class BatchTable : public pdal::FixedPointTable
        {
            const std::function<void(PointBatch &)> &sink_;
            PointBatch batch_;

        public:
            BatchTable(pdal::point_count_t capacity, const std::function<void(PointBatch &)> &sink)
                : pdal::FixedPointTable(capacity), sink_(sink)
            {
                batch_.reserve(capacity);
            }

        protected:
            void reset() override
            {
                using pdal::Dimension::Id;
                const bool hasColor = layout()->hasDim(Id::Red) && layout()->hasDim(Id::Green) &&
                                      layout()->hasDim(Id::Blue);

                batch_.clear();
                pdal::PointRef point(*this, 0);
                for (pdal::PointId idx = 0; idx < numPoints(); idx++)
                {
                    if (skip(idx))
                        continue;
                    point.setPointId(idx);
                    batch_.x.push_back(point.getFieldAs<double>(Id::X));
                    batch_.y.push_back(point.getFieldAs<double>(Id::Y));
                    batch_.z.push_back(point.getFieldAs<double>(Id::Z));
                    batch_.red.push_back(hasColor ? point.getFieldAs<uint16_t>(Id::Red) : 0);
                    batch_.green.push_back(hasColor ? point.getFieldAs<uint16_t>(Id::Green) : 0);
                    batch_.blue.push_back(hasColor ? point.getFieldAs<uint16_t>(Id::Blue) : 0);
                }
                if (batch_.size() > 0)
                    sink_(batch_);

                pdal::FixedPointTable::reset();
            }
        };
    }

    uint64_t CopcHierarchy::totalPoints() const
    {
        uint64_t total = 0;
        for (const auto n : pointsPerDepth)
            total += n;
        return total;
    }

    CopcHierarchy readCopcHierarchy(const std::string &copcPath)
    {
        const std::string path = utils::isNetworkPath(copcPath) ? "/vsicurl/" + copcPath : copcPath;
        VsiFile file(path);

        const auto header = file.read(0, LasHeaderSize + VlrHeaderSize + 160);
        if (std::memcmp(header.data(), "LASF", 4) != 0)
            throw AppException(copcPath + " is not a LAS file");

        // The COPC info VLR immediately follows the LAS 1.4 header
        const uint8_t *vlr = header.data() + LasHeaderSize;
        if (std::strncmp(reinterpret_cast<const char *>(vlr + 2), "copc", 16) != 0 || readLE<uint16_t>(vlr + 18) != 1)
            throw AppException(copcPath + " has no COPC info VLR");

        const uint8_t *info = vlr + VlrHeaderSize;
        CopcHierarchy h;
        h.spacing = readLE<double>(info + 32);
        const auto rootOffset = readLE<uint64_t>(info + 40);
        const auto rootSize = readLE<uint64_t>(info + 48);
        if (!(h.spacing > 0))
            throw AppException(copcPath + " has an invalid COPC spacing");

        std::vector<std::pair<uint64_t, uint64_t>> pages{{rootOffset, rootSize}};
        while (!pages.empty())
        {
            const auto [offset, size] = pages.back();
            pages.pop_back();
            if (size % HierarchyEntrySize != 0 || size > (64u << 20))
                throw AppException(copcPath + " has an invalid hierarchy page");

            const auto page = file.read(offset, static_cast<size_t>(size));
            for (size_t e = 0; e < page.size(); e += HierarchyEntrySize)
            {
                const uint8_t *entry = page.data() + e;
                const auto depth = readLE<int32_t>(entry);
                const auto entryOffset = readLE<uint64_t>(entry + 16);
                const auto byteSize = readLE<int32_t>(entry + 24);
                const auto pointCount = readLE<int32_t>(entry + 28);

                if (depth < 0 || depth > 64)
                    throw AppException(copcPath + " has an invalid hierarchy entry");

                if (pointCount == -1)
                {
                    // Child hierarchy page
                    pages.emplace_back(entryOffset, static_cast<uint64_t>(byteSize));
                }
                else if (pointCount > 0)
                {
                    if (h.pointsPerDepth.size() <= static_cast<size_t>(depth))
                        h.pointsPerDepth.resize(depth + 1, 0);
                    h.pointsPerDepth[depth] += static_cast<uint64_t>(pointCount);
                }
            }
        }

        return h;
    }

    int planCopcDepth(const std::vector<uint64_t> &pointsPerDepth, uint64_t budget)
    {
        int depth = 0;
        uint64_t cumulative = 0;
        for (size_t d = 0; d < pointsPerDepth.size(); d++)
        {
            cumulative += pointsPerDepth[d];
            if (cumulative > budget)
                break;
            depth = static_cast<int>(d);
        }
        return depth;
    }

    double copcResolutionForDepth(double spacing, int depth)
    {
        // log2(spacing / r) = depth - 0.585: halfway (geometrically) between two levels
        return spacing / std::pow(2.0, depth) * 1.5;
    }

    pdal::Options copcReaderOptions(const std::string &copcPath)
    {
        pdal::Options options;
        const fs::path path(copcPath);
        options.add("filename", (!utils::isNetworkPath(copcPath) && path.is_relative()) ? ("." / path).string()
                                                                                         : copcPath);
        options.add("nosrs", true);
        // Reads are LAZ-decompression bound: 4 threads instead of PDAL's 2 for local
        // files cut a z18 tile by a quarter (remote files keep PDAL's 10)
        if (!utils::isNetworkPath(copcPath))
            options.add("threads", static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 4u)));
        return options;
    }

    void forEachPointBatch(pdal::Stage &tail, const std::function<void(PointBatch &)> &sink, size_t chunkSize)
    {
        BatchTable table(chunkSize, sink);
        tail.prepare(table);
        tail.execute(table);
    }
}
