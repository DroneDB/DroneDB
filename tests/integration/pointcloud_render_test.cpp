/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DDB-06 (point-cloud tiles) and DDB-07 (point-cloud thumbnails): shared rasterizer,
// batched reprojection, cached tiler, octree level chosen from a point budget

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>

#include "coordstransformer.h"
#include "copc_utils.h"
#include "pointcloud.h"
#include "point_rasterizer.h"
#include "raster_cache.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"
#include "testfs.h"
#include "tile_test_support.h"
#include "thumbs.h"
#include "tilerhelper.h"

namespace
{
    using namespace ddb;
    using namespace testutils;

    // The point drawing of the tiler before DDB-06, kept as the reference
    void referenceDrawCircle(uint8_t *buffer, uint8_t *alpha, int px, int py, int radius, uint8_t r, uint8_t g,
                             uint8_t b, int tileSize, int wSize)
    {
        const int r2 = radius * radius;
        const int area = r2 << 2;
        const int rr = radius << 1;
        for (int i = 0; i < area; i++)
        {
            const int tx = (i % rr) - radius;
            const int ty = (i / rr) - radius;
            if (tx * tx + ty * ty <= r2)
            {
                const int dx = px + tx;
                const int dy = py + ty;
                if (dx >= 0 && dx < tileSize && dy >= 0 && dy < tileSize)
                {
                    buffer[dy * tileSize + dx + wSize * 0] = r;
                    buffer[dy * tileSize + dx + wSize * 1] = g;
                    buffer[dy * tileSize + dx + wSize * 2] = b;
                    alpha[dy * tileSize + dx] = 255;
                }
            }
        }
    }

    PointBatch randomPoints(size_t n, double minX, double minY, double size, bool sixteenBit, unsigned seed)
    {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<double> pos(-0.05, 1.05);
        std::uniform_real_distribution<double> z(0, 100);
        std::uniform_int_distribution<int> color(0, sixteenBit ? 65535 : 255);
        PointBatch b;
        for (size_t i = 0; i < n; i++)
        {
            b.x.push_back(minX + pos(rng) * size);
            b.y.push_back(minY + pos(rng) * size);
            b.z.push_back(z(rng));
            b.red.push_back(color(rng));
            b.green.push_back(color(rng));
            b.blue.push_back(color(rng));
        }
        return b;
    }

    TEST(DDB06_PointRasterizer, goldenImage)
    {
        const int tileSize = 256;
        const double minX = 1000, minY = 2000, tileMeters = 150;
        const double resolution = tileMeters / tileSize;

        for (const bool sixteenBit : {false, true})
        {
            const auto points = randomPoints(20000, minX, minY, tileMeters, sixteenBit, 42);

            // Reference: the tiler loop before DDB-06
            const int wSize = tileSize * tileSize;
            const int pointRadius = 2;
            const double pointRadiusMeters = pointRadius * resolution;
            const int paddedTileSize = tileSize + pointRadius * 2;
            std::vector<uint8_t> buffer(wSize * 3, 0), alpha(wSize, 0);
            std::vector<float> zBuffer(paddedTileSize * paddedTileSize, -99999.0f);
            const double maxX = minX + tileMeters, maxY = minY + tileMeters;
            const double paddedTileScaleW = paddedTileSize / (maxX - minX + pointRadiusMeters * 2.0);
            const double paddedTileScaleH = paddedTileSize / (maxY - minY + pointRadiusMeters * 2.0);
            for (size_t i = 0; i < points.size(); i++)
            {
                const double x = points.x[i], y = points.y[i], z = points.z[i];
                int px = std::round((x - minX - pointRadiusMeters) * paddedTileScaleW);
                int py = paddedTileSize - 1 - std::round((y - minY + pointRadiusMeters) * paddedTileScaleH);
                if (px >= 0 && px < paddedTileSize && py >= 0 && py < paddedTileSize &&
                    zBuffer[py * paddedTileSize + px] < z)
                {
                    zBuffer[py * paddedTileSize + px] = z;
                    const auto c = [&](uint16_t v) { return sixteenBit ? uint8_t(v >> 8) : uint8_t(v); };
                    referenceDrawCircle(buffer.data(), alpha.data(), px - pointRadius, py - pointRadius, pointRadius,
                                        c(points.red[i]), c(points.green[i]), c(points.blue[i]), tileSize, wSize);
                }
            }

            PixelMapping m;
            m.originX = minX;
            m.shiftX = -pointRadiusMeters;
            m.scaleX = paddedTileScaleW;
            m.originY = minY;
            m.shiftY = pointRadiusMeters;
            m.scaleY = paddedTileScaleH;
            m.width = m.height = paddedTileSize;

            // Two batches: the result must not depend on how points are chunked
            PointRasterizer rasterizer(tileSize, m, pointRadius, pointRadius);
            PointBatch first, second;
            for (size_t i = 0; i < points.size(); i++)
            {
                auto &dst = i < points.size() / 3 ? first : second;
                dst.x.push_back(points.x[i]);
                dst.y.push_back(points.y[i]);
                dst.z.push_back(points.z[i]);
                dst.red.push_back(points.red[i]);
                dst.green.push_back(points.green[i]);
                dst.blue.push_back(points.blue[i]);
            }
            rasterizer.add(first);
            rasterizer.add(second);
            const auto image = rasterizer.toImage();

            EXPECT_EQ(image.color, buffer) << "16-bit: " << sixteenBit;
            EXPECT_EQ(image.alpha, alpha) << "16-bit: " << sixteenBit;
        }
    }

    TEST(DDB06_PointCloudTiler, reprojectionMatchesPerPoint)
    {
        const std::string utm33 =
            "PROJCS[\"WGS 84 / UTM zone 33N\",GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,298.257223563]],"
            "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]],PROJECTION[\"Transverse_Mercator\"],"
            "PARAMETER[\"latitude_of_origin\",0],PARAMETER[\"central_meridian\",15],PARAMETER[\"scale_factor\",0.9996],"
            "PARAMETER[\"false_easting\",500000],PARAMETER[\"false_northing\",0],UNIT[\"metre\",1]]";
        CoordsTransformer ct(utm33, 3857);
        auto batch = randomPoints(5000, 500000, 5000000, 2000, false, 7);
        auto x = batch.x, y = batch.y;
        ct.transform(batch.size(), batch.x.data(), batch.y.data());
        for (size_t i = 0; i < x.size(); i++)
        {
            ct.transform(&x[i], &y[i]);
            ASSERT_DOUBLE_EQ(batch.x[i], x[i]);
            ASSERT_DOUBLE_EQ(batch.y[i], y[i]);
        }
    }

    fs::path brightonCopc(TestArea &ta)
    {
        const auto copc = ta.getPath(fs::path("copc") / "cloud.copc.laz");
        if (!fs::exists(copc))
        {
            const auto pc = ta.downloadTestAsset(
                testDataUrl("brighton/point_cloud.laz"), "point_cloud.laz");
            buildCopc({pc.string()}, ta.getFolder("copc").string());
        }
        return copc;
    }

    TEST(DDB06_PointCloudTiler, cacheHitSkipsCopcInfo)
    {
        TestArea ta(TEST_NAME, true);
        const auto copc = brightonCopc(ta);

        const auto before = RasterCache::instance().stats();
        int opaqueTiles = 0;
        for (int i = 0; i < 5; i++)
        {
            uint8_t *buf = nullptr;
            int size = 0;
            TilerHelper::getTile(inMemory(testTile(copc, 20, 256337, 369481, 256, true, false), &buf, &size));
            const auto img = decodeImage(takeBuffer(buf, size));
            if (std::count(img.alpha().begin(), img.alpha().end(), 255) > 1000)
                opaqueTiles++;
        }
        const auto after = RasterCache::instance().stats();

        EXPECT_EQ(after.created - before.created, 1u) << "one PointCloudTiler (one COPC header read) for 5 tiles";
        EXPECT_EQ(after.reused - before.reused, 4u);
        EXPECT_EQ(opaqueTiles, 5);
    }

    TEST(DDB07_Thumb, plannerPicksDeepestLevelInBudget)
    {
        const std::vector<uint64_t> counts{1000, 4000, 16000, 64000};
        EXPECT_EQ(planCopcDepth(counts, 500), 0) << "the root is always read";
        EXPECT_EQ(planCopcDepth(counts, 5000), 1);
        EXPECT_EQ(planCopcDepth(counts, 20999), 1);
        EXPECT_EQ(planCopcDepth(counts, 21000), 2);
        EXPECT_EQ(planCopcDepth(counts, 1000000), 3);
    }

    TEST(DDB07_Thumb, resolutionSelectsDepth)
    {
        // readers.copc reads the depths below max(1, ceil(log2(spacing / resolution)) + 1)
        for (const double spacing : {0.37, 8.0, 21.5, 1000.0})
        {
            for (int depth = 0; depth < 12; depth++)
            {
                const double r = copcResolutionForDepth(spacing, depth);
                const int depthEnd = std::max(1, static_cast<int>(std::ceil(std::log2(spacing / r))) + 1);
                EXPECT_EQ(depthEnd, depth + 1) << "spacing " << spacing << " depth " << depth;
            }
        }
    }

    uint64_t totalPoints(const CopcHierarchy &h)
    {
        uint64_t total = 0;
        for (const auto n : h.pointsPerDepth)
            total += n;
        return total;
    }

    TEST(DDB07_Thumb, hierarchyCountsAllPoints)
    {
        TestArea ta(TEST_NAME, true);
        const auto copc = brightonCopc(ta);
        PointCloudInfo info;
        ASSERT_TRUE(getCopcInfo(copc.string(), info));
        const auto h = readCopcHierarchy(copc.string());
        EXPECT_GT(h.spacing, 0);
        EXPECT_GE(h.pointsPerDepth.size(), 2u);
        EXPECT_EQ(totalPoints(h), info.pointCount);
    }

    size_t opaquePixels(const DecodedImage &img)
    {
        return img.hasAlpha() ? std::count(img.alpha().begin(), img.alpha().end(), 255)
                              : static_cast<size_t>(img.width) * img.height;
    }

    // The planned read fits the default budget (2 points per pixel) and leaves the
    // deepest level out, and the thumbnail covers what the whole cloud covers
    void expectBudgetedThumbnailComplete(const fs::path &copc, int size)
    {
        const uint64_t budget = 2ull * size * size;
        const auto h = readCopcHierarchy(copc.string());
        const int depth = planCopcDepth(h.pointsPerDepth, budget);
        uint64_t planned = 0;
        for (int d = 0; d <= depth; d++)
            planned += h.pointsPerDepth[d];
        EXPECT_TRUE(planned <= budget || depth == 0);
        EXPECT_LT(depth + 1, static_cast<int>(h.pointsPerDepth.size())) << "the budget leaves deep levels out";

        uint8_t *buf = nullptr;
        int bufSize = 0;
        generatePointCloudThumb(copc, size, "", &buf, &bufSize);
        const auto budgeted = decodeImage(takeBuffer(buf, bufSize));
        generatePointCloudThumb(copc, size, "", &buf, &bufSize, UINT64_MAX);
        const auto full = decodeImage(takeBuffer(buf, bufSize));

        EXPECT_GE(opaquePixels(budgeted), opaquePixels(full) * 95 / 100)
            << "read " << planned << " of " << totalPoints(h) << " points";
    }

    TEST(DDB07_Thumb, pointBudgetRespected)
    {
        TestArea ta(TEST_NAME, true);
        // 24.5k points: a 64 px thumbnail (budget 8192) reads the root level only
        expectBudgetedThumbnailComplete(brightonCopc(ta), 64);

        // Large cloud given by DDB07_LARGE_COPC (e.g. Autzen, 10.6 M points), at 512 px
        const char *large = std::getenv("DDB07_LARGE_COPC");
        if (large != nullptr && fs::exists(large))
            expectBudgetedThumbnailComplete(large, 512);
    }

    struct WarningCounter
    {
        static inline std::atomic<int> warnings{0};
        static void CPL_STDCALL handler(CPLErr level, CPLErrorNum, const char *)
        {
            if (level == CE_Warning || level == CE_Failure)
                warnings++;
        }
    };

    TEST(DDB07_Thumb, noGdalWarnings)
    {
        TestArea ta(TEST_NAME, true);
        const auto copc = brightonCopc(ta);

        WarningCounter::warnings = 0;
        const auto previous = CPLSetErrorHandler(WarningCounter::handler);
        uint8_t *buf = nullptr;
        int bufSize = 0;
        generateThumb(copc, 512, "", true, &buf, &bufSize);
        CPLSetErrorHandler(previous);
        VSIFree(buf);
        EXPECT_EQ(WarningCounter::warnings.load(), 0);
    }

#ifdef __linux__
    long statusKb(const char *field)
    {
        std::ifstream in("/proc/self/status");
        std::string line;
        while (std::getline(in, line))
            if (line.rfind(field, 0) == 0)
                return std::stol(line.substr(std::strlen(field)));
        return -1;
    }

    // Large cloud given by DDB07_LARGE_COPC (e.g. Autzen, 10.6 M points), skipped otherwise
    TEST(DDB07_Thumb, memoryBounded)
    {
        const char *large = std::getenv("DDB07_LARGE_COPC");
        if (large == nullptr || !fs::exists(large))
            GTEST_SKIP() << "Set DDB07_LARGE_COPC to a large .copc.laz";

        // Reset the peak RSS (VmHWM) to the current RSS
        std::ofstream("/proc/self/clear_refs") << "5";
        const long before = statusKb("VmRSS:");
        uint8_t *buf = nullptr;
        int bufSize = 0;
        generateThumb(large, 512, "", true, &buf, &bufSize);
        VSIFree(buf);
        const long peakGrowthMb = (statusKb("VmHWM:") - before) / 1024;
        EXPECT_LE(peakGrowthMb, 100);
    }
#endif
}
