/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Point-cloud tiles and thumbnails: batched reprojection, the tiles of ddb-tilers (cached tiler,
// modern rendering, concurrent tiles, remote clouds) and its thumbnails (a point budget, a remote
// cloud, a cloud without a CRS)

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "coordstransformer.h"
#include "ddbt/ddbt.h"
#include "exceptions.h"
#include "json.h"
#include "pointcloud.h"
#include "point_batch.h"
#include "raster_cache.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"
#include "testfs.h"
#include "tile_test_support.h"
#include "thumbs.h"
#include "tilerhelper.h"
#include "unit/loopback_server.h"

namespace
{
    using namespace ddb;
    using namespace testutils;

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

    TEST(CoordsTransformer, batchMatchesPerPoint)
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

    TEST(PointCloudTiles, cacheHitSkipsCopcInfo)
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

        EXPECT_EQ(after.created - before.created, 1u) << "one tiler (one COPC header read) for 5 tiles";
        EXPECT_EQ(after.reused - before.reused, 4u);
        EXPECT_EQ(opaqueTiles, 5);
    }

    // The tile (z, x, y) of path (a file or a URL), encoded in memory; empty when it is out of the
    // cloud
    std::vector<uint8_t> cloudTile(const fs::path &path, int z, int x, int y, bool forceRecreate = false)
    {
        uint8_t *buf = nullptr;
        int size = 0;
        try
        {
            TilerHelper::getTile(inMemory(testTile(path, z, x, y, 256, true, forceRecreate), &buf, &size));
        }
        catch (const GDALException &)
        {
            return {};
        }
        return takeBuffer(buf, size);
    }

    const std::vector<std::tuple<int, int, int>> &brightonTiles()
    {
        static const std::vector<std::tuple<int, int, int>> tiles = {
            {18, 64084, 92370},   {18, 64085, 92370},   {19, 128168, 184740}, {19, 128169, 184741},
            {20, 256337, 369481}, {20, 256338, 369481}, {17, 32042, 46185}};
        return tiles;
    }

    TEST(PointCloudTiles, concurrentTilesMatchSerial)
    {
        TestArea ta(TEST_NAME, true);
        const auto copc = brightonCopc(ta);
        const auto &tiles = brightonTiles();

        std::vector<std::vector<uint8_t>> serial;
        for (const auto &[z, x, y] : tiles)
            serial.push_back(cloudTile(copc, z, x, y, true));

        std::vector<std::vector<uint8_t>> parallel(tiles.size() * 2);
        std::vector<std::thread> threads;
        for (size_t i = 0; i < parallel.size(); i++)
            threads.emplace_back([&, i]() {
                const auto &[z, x, y] = tiles[i % tiles.size()];
                parallel[i] = cloudTile(copc, z, x, y, true);
            });
        for (auto &t : threads)
            t.join();

        size_t drawn = 0;
        for (size_t i = 0; i < parallel.size(); i++)
        {
            EXPECT_TRUE(parallel[i] == serial[i % tiles.size()]) << "tile " << i % tiles.size();
            drawn += parallel[i].empty() ? 0 : 1;
        }
        EXPECT_GT(drawn, parallel.size() / 2);
    }

    // libddb configures ddb-tilers' modern rendering: its tiles are those of the modern mode, not
    // those of the mode that reproduces libddb 2.4's tiles
    TEST(PointCloudTiles, areRenderedInTheModernMode)
    {
        TestArea ta(TEST_NAME, true);
        const auto copc = brightonCopc(ta);
        const auto &[z, x, y] = brightonTiles()[4];

        const std::vector<uint8_t> tile = cloudTile(copc, z, x, y, true);
        ASSERT_FALSE(tile.empty());
        const auto withMode = [&](const char *mode) {
            const std::string config = json{{"copcTilerMode", mode}}.dump();
            EXPECT_EQ(ddbtConfigure(config.c_str()), DDBT_ERR_NONE) << ddbtLastError();
            return cloudTile(copc, z, x, y, true);
        };
        const std::vector<uint8_t> compat = withMode("compat");
        const std::vector<uint8_t> modern = withMode("modern");
        EXPECT_EQ(tile, modern);
        EXPECT_NE(tile, compat);
    }

#ifndef _WIN32
    // Serves a file with range requests (206 Partial Content) and an ETag, as an object store does
    std::string rangeReply(const std::string &bytes, const std::string &request, const std::string &etag)
    {
        const auto at = request.find("Range: bytes=");
        if (at == std::string::npos)
            return testutils::http(200, "OK", bytes);
        const size_t from = at + 13;
        const size_t dash = request.find('-', from);
        const size_t end = request.find("\r\n", dash);
        const uint64_t first = std::stoull(request.substr(from, dash - from));
        const uint64_t last = (std::min)(std::stoull(request.substr(dash + 1, end - dash - 1)),
                                         static_cast<unsigned long long>(bytes.size() - 1));
        return testutils::http(206, "Partial Content", bytes.substr(first, last - first + 1),
                               "Content-Range: bytes " + std::to_string(first) + "-" + std::to_string(last) + "/" +
                                   std::to_string(bytes.size()) + "\r\nETag: \"" + etag + "\"\r\n");
    }

    TEST(PointCloudTiles, aRemoteCloudIsTiledWithRangeRequestsOfTheHttpClient)
    {
        TestArea ta(TEST_NAME, true);
        const auto copc = brightonCopc(ta);
        std::ifstream in(copc, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        testutils::LoopbackServer server(
            [&](const std::string &request) { return rangeReply(bytes, request, "v1"); });
        const std::string url = server.url("/brighton/cloud.copc.laz");

        // The bytes of the tiles of the local file
        size_t drawn = 0;
        for (const auto &[z, x, y] : brightonTiles())
        {
            const std::vector<uint8_t> remote = cloudTile(url, z, x, y);
            EXPECT_EQ(remote, cloudTile(copc, z, x, y)) << z << "/" << x << "/" << y;
            drawn += remote.empty() ? 0 : 1;
        }
        EXPECT_GT(drawn, brightonTiles().size() / 2);
        const auto requests = server.received();
        ASSERT_FALSE(requests.empty());
        for (const auto &request : requests)
            EXPECT_NE(request.find("Range: bytes="), std::string::npos) << request;

        // A server that ignores the range is refused with the reason, and is not read whole
        testutils::LoopbackServer whole([&](const std::string &) { return testutils::http(200, "OK", bytes.substr(0, 4096)); });
        uint8_t *buf = nullptr;
        int size = 0;
        try
        {
            TilerHelper::getTile(inMemory(testTile(whole.url("/cloud.copc.laz"), 20, 256337, 369481, 256, true, false),
                                          &buf, &size));
            FAIL() << "a server without range requests";
        }
        catch (const AppException &e)
        {
            EXPECT_NE(std::string(e.what()).find("does not answer range requests"), std::string::npos) << e.what();
        }
        EXPECT_EQ(buf, nullptr);

        // A missing cloud
        testutils::LoopbackServer missing([](const std::string &) { return testutils::http(404, "Not Found", ""); });
        EXPECT_THROW(cloudTile(missing.url("/cloud.copc.laz"), 20, 256337, 369481), FSException);
    }
#endif

    size_t opaquePixels(const DecodedImage &img)
    {
        return img.hasAlpha() ? std::count(img.alpha().begin(), img.alpha().end(), 255)
                              : static_cast<size_t>(img.width) * img.height;
    }

    // A thumbnail read within its default budget (2 points per pixel, ddb-tilers' planner) covers what
    // a thumbnail of every point covers
    void expectBudgetedThumbnailComplete(const fs::path &copc, int size)
    {
        uint8_t *buf = nullptr;
        int bufSize = 0;
        generatePointCloudThumb(copc, size, "", &buf, &bufSize);
        const auto budgeted = decodeImage(takeBuffer(buf, bufSize));
        generatePointCloudThumb(copc, size, "", &buf, &bufSize, UINT64_MAX);
        const auto full = decodeImage(takeBuffer(buf, bufSize));

        EXPECT_EQ(budgeted.width, size);
        EXPECT_EQ(budgeted.height, size);
        EXPECT_GT(opaquePixels(full), static_cast<size_t>(size) * size / 4);
        EXPECT_GE(opaquePixels(budgeted), opaquePixels(full) * 95 / 100);
    }

#ifndef _WIN32
    // A thumbnail of a COPC at a URL is read with the range requests of the HTTP client
    TEST(PointCloudThumbBudget, aRemoteCloudHasTheThumbnailOfTheFile)
    {
        TestArea ta(TEST_NAME, true);
        const auto copc = brightonCopc(ta);
        std::ifstream in(copc, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        testutils::LoopbackServer server(
            [&](const std::string &request) { return rangeReply(bytes, request, "v1"); });

        uint8_t *buf = nullptr;
        int size = 0;
        generatePointCloudThumb(copc, 128, "", &buf, &size);
        const std::vector<uint8_t> local = takeBuffer(buf, size);
        generatePointCloudThumb(server.url("/brighton/cloud.copc.laz"), 128, "", &buf, &size);
        EXPECT_EQ(takeBuffer(buf, size), local);
    }
#endif

    TEST(PointCloudThumbBudget, pointBudgetRespected)
    {
        TestArea ta(TEST_NAME, true);
        // 24.5k points: a 64 px thumbnail (budget 8192) reads the shallow levels only
        expectBudgetedThumbnailComplete(brightonCopc(ta), 64);

        // Large cloud given by DDB_TEST_LARGE_COPC (e.g. Autzen, 10.6 M points), at 512 px
        const char *large = std::getenv("DDB_TEST_LARGE_COPC");
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

    TEST(PointCloudThumbBudget, noGdalWarnings)
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

    // Large cloud given by DDB_TEST_LARGE_COPC (e.g. Autzen, 10.6 M points), skipped otherwise
    TEST(PointCloudThumbBudget, memoryBounded)
    {
        const char *large = std::getenv("DDB_TEST_LARGE_COPC");
        if (large == nullptr || !fs::exists(large))
            GTEST_SKIP() << "Set DDB_TEST_LARGE_COPC to a large .copc.laz";

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
