/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "copc_utils.h"

#include <cmath>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <algorithm>

#include "pdal_inc.h"
#include <pdal/Options.hpp>
#include <pdal/PointTable.hpp>
#include <pdal/PointRef.hpp>
#include <pdal/Stage.hpp>
#include <pdal/io/CopcReader.hpp>

#include <lazperf/readers.hpp>

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

    uint64_t CopcHierarchy::totalPoints() const
    {
        uint64_t total = 0;
        for (const auto n : pointsPerDepth)
            total += n;
        return total;
    }

    CopcHierarchy CopcIndex::hierarchy() const
    {
        CopcHierarchy h;
        h.spacing = spacing;
        for (const auto &node : nodes)
        {
            if (h.pointsPerDepth.size() <= static_cast<size_t>(node.key.d))
                h.pointsPerDepth.resize(node.key.d + 1, 0);
            h.pointsPerDepth[node.key.d] += static_cast<uint64_t>(node.pointCount);
        }
        return h;
    }

    void CopcIndex::nodeBounds(const CopcNodeKey &key, double &minX, double &minY, double &maxX,
                               double &maxY) const
    {
        // Same arithmetic as PDAL, so a node is selected exactly when readers.copc
        // selects it
        const double rootMinX = centerX - halfSize, rootMaxX = centerX + halfSize;
        const double rootMinY = centerY - halfSize, rootMaxY = centerY + halfSize;
        const int width = static_cast<int>(std::pow(2, key.d));
        const double cellWidth = (rootMaxX - rootMinX) / width;
        minX = key.x == 0 ? rootMinX : rootMinX + cellWidth * key.x;
        maxX = key.x == width - 1 ? rootMaxX : rootMinX + cellWidth * (key.x + 1);
        minY = key.y == 0 ? rootMinY : rootMinY + cellWidth * key.y;
        maxY = key.y == width - 1 ? rootMaxY : rootMinY + cellWidth * (key.y + 1);
    }

    CopcHierarchy readCopcHierarchy(const std::string &copcPath)
    {
        return readCopcIndex(copcPath).hierarchy();
    }

    CopcIndex readCopcIndex(const std::string &copcPath)
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

        CopcIndex h;
        h.pointFormat = header[104] & 0x3F; // bits 6-7 flag compression
        h.pointSize = readLE<uint16_t>(header.data() + 105);
        for (int i = 0; i < 3; i++)
        {
            h.scale[i] = readLE<double>(header.data() + 131 + 8 * i);
            h.offset[i] = readLE<double>(header.data() + 155 + 8 * i);
        }

        const uint8_t *info = vlr + VlrHeaderSize;
        h.centerX = readLE<double>(info);
        h.centerY = readLE<double>(info + 8);
        h.centerZ = readLE<double>(info + 16);
        h.halfSize = readLE<double>(info + 24);
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
                CopcNode node;
                node.key = {readLE<int32_t>(entry), readLE<int32_t>(entry + 4), readLE<int32_t>(entry + 8),
                            readLE<int32_t>(entry + 12)};
                node.offset = readLE<uint64_t>(entry + 16);
                node.byteSize = readLE<int32_t>(entry + 24);
                node.pointCount = readLE<int32_t>(entry + 28);

                if (node.key.d < 0 || node.key.d > 64)
                    throw AppException(copcPath + " has an invalid hierarchy entry");

                if (node.pointCount == -1)
                {
                    // Child hierarchy page
                    pages.emplace_back(node.offset, static_cast<uint64_t>(node.byteSize));
                }
                else if (node.pointCount > 0)
                {
                    h.nodes.push_back(node);
                }
            }
        }

        return h;
    }

    void decodeCopcNode(const std::string &copcPath, const CopcIndex &index, const CopcNode &node,
                        PointBatch &points)
    {
        // Point formats 6-8 share the first 30 bytes; 7 and 8 add RGB at byte 30
        static const int baseSize[] = {20, 28, 26, 34, 57, 63, 30, 36, 38, 59, 67};
        if (index.pointFormat < 6 || index.pointFormat > 8 || index.pointSize < baseSize[index.pointFormat])
            throw AppException(copcPath + " has an unsupported point format " + std::to_string(index.pointFormat));
        const int ebCount = index.pointSize - baseSize[index.pointFormat];

        const std::string path = io::isNetworkPath(copcPath) ? "/vsicurl/" + copcPath : copcPath;
        auto chunk = VsiFile(path).read(node.offset, static_cast<size_t>(node.byteSize));

        points.clear();
        points.reserve(static_cast<size_t>(node.pointCount));
        try
        {
            lazperf::reader::chunk_decompressor decompressor(index.pointFormat, ebCount,
                                                             reinterpret_cast<const char *>(chunk.data()));
            std::vector<char> record(static_cast<size_t>(index.pointSize));
            const auto *p = reinterpret_cast<const uint8_t *>(record.data());
            const bool color = index.hasColor();
            for (int32_t i = 0; i < node.pointCount; i++)
            {
                decompressor.decompress(record.data());
                points.x.push_back(readLE<int32_t>(p) * index.scale[0] + index.offset[0]);
                points.y.push_back(readLE<int32_t>(p + 4) * index.scale[1] + index.offset[1]);
                points.z.push_back(readLE<int32_t>(p + 8) * index.scale[2] + index.offset[2]);
                points.red.push_back(color ? readLE<uint16_t>(p + 30) : 0);
                points.green.push_back(color ? readLE<uint16_t>(p + 32) : 0);
                points.blue.push_back(color ? readLE<uint16_t>(p + 34) : 0);
            }
        }
        catch (const std::exception &e)
        {
            throw AppException("Cannot decompress COPC node " + std::to_string(node.key.d) + "-" +
                               std::to_string(node.key.x) + "-" + std::to_string(node.key.y) + "-" +
                               std::to_string(node.key.z) + " of " + copcPath + ": " + e.what());
        }
    }

    int copcDepthEnd(double spacing, double resolution)
    {
        return (std::max)(1, static_cast<int>(std::ceil(std::log2(spacing / resolution))) + 1);
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

    pdal::Options copcWindowOptions(const std::string &copcPath, const parallel::CpuTokens &threads,
                                    const CopcWindow &window)
    {
        pdal::Options options = copcReaderOptions(copcPath, threads);
        std::ostringstream bounds;
        bounds << std::setprecision(17) << "([" << window.minX << "," << window.maxX << "],[" << window.minY << ","
               << window.maxY << "])";
        options.add("bounds", bounds.str());
        options.add("resolution", window.resolution);
        return options;
    }

    void readCopcWindow(const std::string &copcPath, const CopcWindow &window,
                        const std::function<void(PointBatch &)> &sink)
    {
        const auto threads = copcReadThreads(copcPath);
        pdal::CopcReader reader;
        reader.setOptions(copcWindowOptions(copcPath, threads, window));
        forEachPointBatch(reader, sink);
    }

    void forEachPointBatch(pdal::Stage &tail, const std::function<void(PointBatch &)> &sink, size_t chunkSize)
    {
        BatchTable table(chunkSize, sink);
        tail.prepare(table);
        tail.execute(table);
    }
}
