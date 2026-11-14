/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "copc_utils.h"

#include <cmath>
#include <cstring>
#include <algorithm>

#include "pdal_inc.h"
#include <pdal/Options.hpp>
#include <pdal/PointTable.hpp>
#include <pdal/PointRef.hpp>
#include <pdal/Stage.hpp>
#include <pdal/io/CopcReader.hpp>

#include "exceptions.h"
#include "gdal_raii.h"
#include "mio.h"

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
            VsiFilePtr f_;
            std::string path_;

        public:
            explicit VsiFile(const std::string &path)
                : f_(VSIFOpenL(path.c_str(), "rb")), path_(path)
            {
                if (!f_)
                    throw AppException("Cannot open " + path);
            }

            std::vector<uint8_t> read(uint64_t offset, size_t size)
            {
                std::vector<uint8_t> buf(size);
                if (VSIFSeekL(f_.get(), offset, SEEK_SET) != 0 ||
                    VSIFReadL(buf.data(), 1, size, f_.get()) != size)
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

    CopcHierarchy readCopcHierarchy(const std::string &copcPath)
    {
        const std::string path = io::isNetworkPath(copcPath) ? "/vsicurl/" + copcPath : copcPath;
        VsiFile file(path);

        const auto header = file.read(0, LasHeaderSize + VlrHeaderSize + 160);
        if (std::memcmp(header.data(), "LASF", 4) != 0)
            throw AppException(copcPath + " is not a LAS file");

        // The COPC info VLR immediately follows the LAS 1.4 header
        const uint8_t *vlr = header.data() + LasHeaderSize;
        if (std::strncmp(reinterpret_cast<const char *>(vlr + 2), "copc", 16) != 0 || readLE<uint16_t>(vlr + 18) != 1)
            throw AppException(copcPath + " has no COPC info VLR");

        CopcHierarchy h;
        const uint8_t *info = vlr + VlrHeaderSize;
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
                // Entry: key (depth, x, y, z), offset, byte size, point count (-1: a child page)
                const uint8_t *entry = page.data() + e;
                const auto depth = readLE<int32_t>(entry);
                const auto pointCount = readLE<int32_t>(entry + 28);

                if (depth < 0 || depth > 64)
                    throw AppException(copcPath + " has an invalid hierarchy entry");

                if (pointCount == -1)
                {
                    pages.emplace_back(readLE<uint64_t>(entry + 16),
                                       static_cast<uint64_t>(readLE<int32_t>(entry + 24)));
                }
                else if (pointCount > 0)
                {
                    if (h.pointsPerDepth.size() <= static_cast<size_t>(depth))
                        h.pointsPerDepth.resize(static_cast<size_t>(depth) + 1, 0);
                    h.pointsPerDepth[static_cast<size_t>(depth)] += static_cast<uint64_t>(pointCount);
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

    parallel::CpuTokens copcReadThreads(const std::string &copcPath)
    {
        // Reads are LAZ-decompression bound: 4 threads instead of PDAL's 2 for local
        // files cut a z18 tile by a quarter (remote files keep PDAL's 10)
        constexpr size_t LocalReadThreads = 4;
        return io::isNetworkPath(copcPath) ? parallel::CpuTokens()
                                              : parallel::tryAcquireThreads(LocalReadThreads);
    }

    pdal::Options copcReaderOptions(const std::string &copcPath, const parallel::CpuTokens &threads)
    {
        pdal::Options options;
        const fs::path path = io::fromUtf8(copcPath);
        options.add("filename", (!io::isNetworkPath(copcPath) && path.is_relative()) ? io::toUtf8("." / path)
                                                                                         : copcPath);
        options.add("nosrs", true);
        if (!io::isNetworkPath(copcPath))
            options.add("threads", static_cast<int>(threads.threads()));
        return options;
    }

    void forEachPointBatch(pdal::Stage &tail, const std::function<void(PointBatch &)> &sink, size_t chunkSize)
    {
        BatchTable table(chunkSize, sink);
        tail.prepare(table);
        tail.execute(table);
    }
}
