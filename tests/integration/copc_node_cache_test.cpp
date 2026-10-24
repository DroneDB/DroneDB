/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DDB-06: point-cloud tiles read COPC nodes through a shared cache of decoded nodes and
// get exactly the points readers.copc returns for the same bounds and resolution

#include <algorithm>
#include <cstdlib>
#include <thread>
#include <tuple>

#include "copc_node_cache.h"
#include "copc_utils.h"
#include "pointcloud.h"
#include "raster_cache.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"
#include "testfs.h"
#include "tile_test_support.h"
#include "tilerhelper.h"

namespace
{
    using namespace ddb;
    using namespace testutils;

    using Point = std::tuple<double, double, double, uint16_t, uint16_t, uint16_t>;

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

    void append(std::vector<Point> &out, const PointBatch &b)
    {
        for (size_t i = 0; i < b.size(); i++)
            out.emplace_back(b.x[i], b.y[i], b.z[i], b.red[i], b.green[i], b.blue[i]);
    }

    struct Window
    {
        double minX, minY, maxX, maxY;
    };

    std::vector<Point> readWithNodes(const CopcNodeReader &reader, const Window &w, double resolution)
    {
        std::vector<Point> points;
        reader.read(w.minX, w.minY, w.maxX, w.maxY, resolution, [&](PointBatch &b) { append(points, b); });
        std::sort(points.begin(), points.end());
        return points;
    }

    std::vector<Point> readWithPdal(const fs::path &copc, const Window &w, double resolution)
    {
        std::vector<Point> points;
        readCopcWindow(copc.string(), w.minX, w.minY, w.maxX, w.maxY, resolution,
                       [&](PointBatch &b) { append(points, b); });
        std::sort(points.begin(), points.end());
        return points;
    }

    // Extent of the points (the octree cube is larger)
    Window dataExtent(const CopcNodeReader &reader)
    {
        Window w{1e300, 1e300, -1e300, -1e300};
        const auto &idx = reader.index();
        reader.read(idx.centerX - idx.halfSize, idx.centerY - idx.halfSize, idx.centerX + idx.halfSize,
                    idx.centerY + idx.halfSize, idx.spacing, [&](PointBatch &b) {
                        for (size_t i = 0; i < b.size(); i++)
                        {
                            w.minX = std::min(w.minX, b.x[i]);
                            w.maxX = std::max(w.maxX, b.x[i]);
                            w.minY = std::min(w.minY, b.y[i]);
                            w.maxY = std::max(w.maxY, b.y[i]);
                        }
                    });
        return w;
    }

    void expectSamePointsAsReadersCopc(const fs::path &copc)
    {
        CopcNodeCache::instance().clear();
        const CopcNodeReader reader(copc.string());
        ASSERT_TRUE(reader.hasColor());

        const Window all = dataExtent(reader);
        const double w = all.maxX - all.minX, h = all.maxY - all.minY;
        const std::vector<Window> windows = {
            all,
            {all.minX + w * 0.25, all.minY + h * 0.25, all.minX + w * 0.5, all.minY + h * 0.5},
            {all.minX + w * 0.6, all.minY + h * 0.1, all.minX + w * 0.62, all.minY + h * 0.13},
            {all.maxX + 10, all.maxY + 10, all.maxX + 20, all.maxY + 20}, // outside: no points
        };
        const double spacing = reader.index().spacing;
        for (const auto &win : windows)
        {
            for (const double resolution : {spacing * 2, spacing / 3, spacing / 20, spacing / 500})
            {
                const auto nodes = readWithNodes(reader, win, resolution);
                const auto pdal = readWithPdal(copc, win, resolution);
                EXPECT_EQ(nodes.size(), pdal.size()) << "resolution " << resolution;
                EXPECT_TRUE(nodes == pdal) << "resolution " << resolution;
            }
        }
    }

    TEST(DDB06_NodeReader, samePointsAsReadersCopc)
    {
        TestArea ta(TEST_NAME, true);
        expectSamePointsAsReadersCopc(brightonCopc(ta));

        // Any other coloured COPC, e.g. the bench cloud (autzen-classified.copc.laz)
        if (const char *extra = std::getenv("DDB06_EXTRA_COPC"))
            expectSamePointsAsReadersCopc(extra);
    }

    TEST(DDB06_NodeCache, hitDecodesNothing)
    {
        TestArea ta(TEST_NAME, true);
        const auto copc = brightonCopc(ta);
        CopcNodeCache::instance().clear();
        const CopcNodeReader reader(copc.string());
        const auto &idx = reader.index();
        const Window cube{idx.centerX - idx.halfSize, idx.centerY - idx.halfSize, idx.centerX + idx.halfSize,
                          idx.centerY + idx.halfSize};

        const auto loads0 = CopcNodeCache::instance().loads();
        const auto first = readWithNodes(reader, cube, idx.spacing / 4);
        const auto loads1 = CopcNodeCache::instance().loads();
        const auto second = readWithNodes(reader, cube, idx.spacing / 4);
        EXPECT_GT(loads1 - loads0, 1u);
        EXPECT_EQ(CopcNodeCache::instance().loads(), loads1) << "a cache hit decompresses nothing";
        EXPECT_TRUE(first == second);
        EXPECT_GT(CopcNodeCache::instance().bytes(), 0u);
    }

    TEST(DDB06_NodeCache, concurrentReadersDecodeEachNodeOnce)
    {
        TestArea ta(TEST_NAME, true);
        const auto copc = brightonCopc(ta);
        CopcNodeCache::instance().clear();
        const CopcNodeReader reader(copc.string());
        const auto &idx = reader.index();
        const Window cube{idx.centerX - idx.halfSize, idx.centerY - idx.halfSize, idx.centerX + idx.halfSize,
                          idx.centerY + idx.halfSize};
        const double resolution = idx.spacing / 4;
        const size_t nodes = std::count_if(idx.nodes.begin(), idx.nodes.end(), [&](const CopcNode &n) {
            return n.key.d < copcDepthEnd(idx.spacing, resolution);
        });

        const auto loads0 = CopcNodeCache::instance().loads();
        std::vector<std::vector<Point>> results(8);
        std::vector<std::thread> threads;
        for (size_t t = 0; t < results.size(); t++)
            threads.emplace_back([&, t]() { results[t] = readWithNodes(reader, cube, resolution); });
        for (auto &t : threads)
            t.join();

        EXPECT_EQ(CopcNodeCache::instance().loads() - loads0, nodes);
        for (const auto &r : results)
            EXPECT_TRUE(r == results[0]);
    }

    TEST(DDB06_NodeCache, staysWithinItsCapacity)
    {
        TestArea ta(TEST_NAME, true);
        const auto copc = brightonCopc(ta);
        auto &cache = CopcNodeCache::instance();
        cache.clear();
        const CopcNodeReader reader(copc.string());
        const auto &idx = reader.index();

        const size_t capacity = 256 << 10;
        cache.setCapacity(capacity);
        const auto full = readWithNodes(reader, {idx.centerX - idx.halfSize, idx.centerY - idx.halfSize,
                                                 idx.centerX + idx.halfSize, idx.centerY + idx.halfSize},
                                        idx.spacing / 1000);
        EXPECT_LE(cache.bytes(), capacity);
        EXPECT_GT(full.size() * 30, capacity) << "the read itself needed more than the capacity";
        cache.setCapacity(256u << 20);
    }

    TEST(DDB06_NodeCache, rewrittenCloudIsReadAgainAndReleaseDropsIt)
    {
        TestArea ta(TEST_NAME, true);
        const auto copc = brightonCopc(ta);
        auto &cache = CopcNodeCache::instance();
        cache.clear();
        const auto copy = ta.getPath(fs::path("other") / "cloud.copc.laz");
        fs::create_directories(copy.parent_path());
        fs::copy_file(copc, copy);

        const auto readAll = [](const fs::path &path) {
            const CopcNodeReader reader(path.string());
            const auto &idx = reader.index();
            return readWithNodes(reader, {idx.centerX - idx.halfSize, idx.centerY - idx.halfSize,
                                          idx.centerX + idx.halfSize, idx.centerY + idx.halfSize},
                                 idx.spacing);
        };

        readAll(copy);
        const auto loads = cache.loads();
        readAll(copy);
        EXPECT_EQ(cache.loads(), loads);

        // A rebuilt file (new version) never gets the old nodes
        fs::last_write_time(copy, fs::last_write_time(copy) + std::chrono::seconds(5));
        readAll(copy);
        EXPECT_GT(cache.loads(), loads);

        EXPECT_GT(cache.bytes(), 0u);
        releaseCachedFiles(copy.parent_path());
        EXPECT_EQ(cache.bytes(), 0u);
    }

    TEST(DDB06_PointCloudTiler, concurrentTilesMatchSerial)
    {
        TestArea ta(TEST_NAME, true);
        const auto copc = brightonCopc(ta);
        CopcNodeCache::instance().clear();

        const std::vector<std::tuple<int, int, int>> tiles = {
            {18, 64084, 92370}, {18, 64085, 92370}, {19, 128168, 184740}, {19, 128169, 184741},
            {20, 256337, 369481}, {20, 256338, 369481}, {17, 32042, 46185}};
        const auto render = [&](int z, int x, int y) {
            uint8_t *buf = nullptr;
            int size = 0;
            try
            {
                TilerHelper::getTile(inMemory(testTile(copc, z, x, y, 256, true, false), &buf, &size));
            }
            catch (const GDALException &)
            {
                return std::vector<uint8_t>(); // outside the cloud
            }
            return takeBuffer(buf, size);
        };

        std::vector<std::vector<uint8_t>> serial;
        for (const auto &[z, x, y] : tiles)
            serial.push_back(render(z, x, y));

        CopcNodeCache::instance().clear();
        std::vector<std::vector<uint8_t>> parallel(tiles.size() * 2);
        std::vector<std::thread> threads;
        for (size_t i = 0; i < parallel.size(); i++)
            threads.emplace_back([&, i]() {
                const auto &[z, x, y] = tiles[i % tiles.size()];
                parallel[i] = render(z, x, y);
            });
        for (auto &t : threads)
            t.join();

        for (size_t i = 0; i < parallel.size(); i++)
            EXPECT_TRUE(parallel[i] == serial[i % tiles.size()]) << "tile " << i % tiles.size();
    }
}
