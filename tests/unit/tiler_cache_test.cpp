/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Raster tiles reuse a prepared tiler, compute statistics once and encode
// through TileEncoder

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include "ddb.h"
#include "gdaltiler.h"
#include "mio.h"
#include "raster_cache.h"
#include "raster_stats.h"
#include "raster_test_utils.h"
#include "sensorprofile.h"
#include "test.h"
#include "testarea.h"
#include "tile_encoder.h"
#include "tile_test_support.h"
#include "tilerhelper.h"
#include "userprofile.h"
#include "utils.h"

namespace
{
    using namespace ddb;
    using namespace testutils;

    struct TileRef
    {
        int z, x, y;
    };

    // Every tile of the deepest zoom of the raster (XYZ)
    std::vector<TileRef> maxZoomTiles(const fs::path &raster)
    {
        GDALTiler t(TilerHelper::toGeoTIFF(raster).string(), "", 256);
        const int z = t.getMinMaxZ().max;
        auto b = t.getMinMaxCoordsForZ(z);
        std::vector<TileRef> tiles;
        for (int x = b.min.x; x <= b.max.x; x++)
            for (int y = b.min.y; y <= b.max.y; y++)
                tiles.push_back({z, x, y});
        return tiles;
    }

    std::vector<uint8_t> memoryTile(const fs::path &raster, const TileRef &t, const std::string &format = "png")
    {
        uint8_t *buf = nullptr;
        int size = 0;
        auto request = inMemory(testTile(raster, t.z, t.x, t.y, 256, false, false), &buf, &size);
        request.format = format;
        TilerHelper::getTile(request);
        return takeBuffer(buf, size);
    }

    // A tile of a raster that is fully covered by data
    TileRef fullTile(const fs::path &raster)
    {
        for (const auto &t : maxZoomTiles(raster))
        {
            const auto img = decodeImage(memoryTile(raster, t));
            if (!img.hasAlpha())
                return t;
        }
        throw std::runtime_error("No fully covered tile");
    }

    GeoRasterSpec rgbGradient()
    {
        GeoRasterSpec spec;
        spec.width = 1024;
        spec.height = 1024;
        spec.value = [](int band, int x, int y) { return static_cast<double>((x * band + y) % 256); };
        return spec;
    }

    GeoRasterSpec floatRamp(double from, double to)
    {
        GeoRasterSpec spec;
        spec.bands = 1;
        spec.type = GDT_Float32;
        spec.value = [=](int, int x, int) { return from + (to - from) * x / 511.0; };
        return spec;
    }

    int maxOfBand(const DecodedImage &img, int band)
    {
        return *std::max_element(img.bands[band].begin(), img.bands[band].end());
    }

    TEST(TilerCache, cacheHitSkipsOpen)
    {
        TestArea ta(TEST_NAME, true);
        auto spec = rgbGradient();
        spec.epsg = 32633; // UTM: the tiler builds a warped VRT
        spec.originX = 500000;
        spec.originY = 5000000;
        const auto raster = createGeoRaster(ta.getPath("utm.tif"), spec);
        const auto tiles = maxZoomTiles(raster);
        ASSERT_GE(tiles.size(), 2u);

        {
            GdalOpenCounter opens;
            memoryTile(raster, tiles[0]);
            EXPECT_GE(opens.count(), 1) << "the first tile opens the raster";
        }

        const auto before = RasterCache::instance().stats();
        GdalOpenCounter opens;
        memoryTile(raster, tiles[1]);
        memoryTile(raster, tiles[0]);
        EXPECT_EQ(opens.count(), 0);
        EXPECT_EQ(RasterCache::instance().stats().created, before.created);
        EXPECT_GE(RasterCache::instance().stats().reused, before.reused + 2);
    }

    TEST(TilerCache, statsComputedOnce)
    {
        TestArea ta(TEST_NAME, true);
        const auto raster = createGeoRaster(ta.getPath("float.tif"), floatRamp(10, 500));
        const auto tiles = maxZoomTiles(raster);

        const auto before = RasterStatsProvider::instance().counters();
        for (int i = 0; i < 50; i++)
            memoryTile(raster, tiles[i % tiles.size()]);
        const auto after = RasterStatsProvider::instance().counters();

        EXPECT_EQ(after.computed - before.computed, 1u);
        EXPECT_EQ(after.fromSidecar, before.fromSidecar);
    }

    TEST(TilerCache, sidecarUsed)
    {
        TestArea ta(TEST_NAME, true);
        const auto raster = createGeoRaster(ta.getPath("float.tif"), floatRamp(0, 500));
        // The COG the tiles come from has the sidecar the COG build writes; here it claims a
        // range twice the data's
        const auto cog = TilerHelper::toGeoTIFF(raster);
        std::ofstream(cog.string() + ".stats.json")
            << R"({"bands":{"1":{"min":0,"max":1000,"mean":500,"std":100,"p2":20,"p98":980}}})";

        const auto before = RasterStatsProvider::instance().counters();
        int brightest = 0;
        for (const auto &t : maxZoomTiles(raster))
            brightest = std::max(brightest, maxOfBand(decodeImage(memoryTile(raster, t)), 0));
        const auto after = RasterStatsProvider::instance().counters();

        EXPECT_EQ(after.computed, before.computed);
        EXPECT_EQ(after.fromSidecar - before.fromSidecar, 1u);
        EXPECT_NEAR(brightest, 127, 2) << "500 of [0, 1000] is mid grey";
    }

    TEST(TilerCache, concurrentTilesSameRaster)
    {
        TestArea ta(TEST_NAME, true);
        const auto raster = createGeoRaster(ta.getPath("rgb.tif"), rgbGradient());
        const auto tiles = maxZoomTiles(raster);

        std::vector<std::vector<uint8_t>> serial;
        for (const auto &t : tiles)
            serial.push_back(memoryTile(raster, t));

        std::atomic<int> mismatches{0};
        std::atomic<int> failures{0};
        std::vector<std::thread> threads;
        for (int th = 0; th < 16; th++)
        {
            threads.emplace_back([&, th]() {
                for (int i = 0; i < 100; i++)
                {
                    const size_t k = (th * 7 + i) % tiles.size();
                    try
                    {
                        if (memoryTile(raster, tiles[k]) != serial[k])
                            mismatches++;
                    }
                    catch (...)
                    {
                        failures++;
                    }
                }
            });
        }
        for (auto &t : threads)
            t.join();

        EXPECT_EQ(failures.load(), 0);
        EXPECT_EQ(mismatches.load(), 0);
    }

    TEST(TilerCache, evictOnRebuild)
    {
        TestArea ta(TEST_NAME, true);
        auto spec = rgbGradient();
        spec.value = [](int, int, int) { return 10.0; };
        const auto raster = createGeoRaster(ta.getPath("rebuilt.tif"), spec);
        const auto tile = fullTile(raster);
        // The tiles come from a JPEG compressed COG: the values are near the ones written
        EXPECT_NEAR(maxOfBand(decodeImage(memoryTile(raster, tile)), 0), 10, 3);

        // Rebuilt in place, as a COG build replaces its output
        spec.value = [](int, int, int) { return 200.0; };
        createGeoRaster(raster, spec);
        EXPECT_NEAR(maxOfBand(decodeImage(memoryTile(raster, tile)), 0), 200, 3);
    }

    TEST(TilerCache, releaseClosesIdleRasters)
    {
        TestArea ta(TEST_NAME, true);
        const auto raster = createGeoRaster(ta.getPath("rgb.tif"), rgbGradient());
        memoryTile(raster, maxZoomTiles(raster)[0]);

        EXPECT_GE(RasterCache::instance().release(ta.getFolder()), 1u);
        EXPECT_EQ(RasterCache::instance().release(ta.getFolder()), 0u);

        int closed = 0;
        memoryTile(raster, maxZoomTiles(raster)[0]);
        ASSERT_EQ(DDBReleaseConnections(ta.getFolder().string().c_str(), &closed), DDBERR_NONE);
        EXPECT_GE(closed, 1);
    }

    TEST(TilerCache, opaqueTileHasNoAlpha)
    {
        TestArea ta(TEST_NAME, true);
        const auto raster = createGeoRaster(ta.getPath("rgb.tif"), rgbGradient());
        const auto img = decodeImage(memoryTile(raster, fullTile(raster)));
        EXPECT_EQ(img.bands.size(), 3u);

        // An edge tile keeps its alpha
        bool edgeWithAlpha = false;
        for (const auto &t : maxZoomTiles(raster))
            edgeWithAlpha |= decodeImage(memoryTile(raster, t)).hasAlpha();
        EXPECT_TRUE(edgeWithAlpha);
    }

    TEST(TilerCache, webpAndJpegTiles)
    {
        TestArea ta(TEST_NAME, true);
        const auto raster = createGeoRaster(ta.getPath("rgb.tif"), rgbGradient());
        const auto tile = fullTile(raster);

        const auto webp = memoryTile(raster, tile, "webp");
        ASSERT_GT(webp.size(), 12u);
        EXPECT_EQ(std::string(webp.begin(), webp.begin() + 4), "RIFF");
        EXPECT_EQ(std::string(webp.begin() + 8, webp.begin() + 12), "WEBP");
        EXPECT_EQ(decodeImage(webp).width, 256);

        uint8_t *buf = nullptr;
        int size = 0;
        ASSERT_EQ(DDBMemoryTileFmt(raster.string().c_str(), tile.z, tile.x, tile.y, &buf, &size, 256, false, false, "",
                                   "image/jpeg"),
                  DDBERR_NONE);
        const auto jpeg = takeBuffer(buf, size);
        EXPECT_EQ(jpeg[0], 0xFF);
        EXPECT_EQ(jpeg[1], 0xD8);
    }

    TEST(TilerCache, blankTilesShareTheirBytes)
    {
        TestArea ta(TEST_NAME, true);
        const auto raster = createGeoRaster(ta.getPath("rgb.tif"), rgbGradient());
        const auto a = memoryTile(raster, {1, 0, 0});
        const auto b = memoryTile(raster, {2, 3, 3});
        EXPECT_EQ(a, b);
        const auto img = decodeImage(a);
        ASSERT_TRUE(img.hasAlpha());
        EXPECT_EQ(maxOfBand(img, static_cast<int>(img.bands.size()) - 1), 0);
    }

    TEST(TilerCache, sensorDetectionIsMemoised)
    {
        TestArea ta(TEST_NAME, true);
        GeoRasterSpec spec;
        spec.bands = 5;
        spec.type = GDT_UInt16;
        spec.width = spec.height = 64;
        spec.value = [](int band, int x, int) { return 1000.0 * band + x; };
        const auto raster = createGeoRaster(ta.getPath("ms.tif"), spec);

        auto &spm = SensorProfileManager::instance();
        const auto before = spm.uncachedDetections();
        const auto first = spm.detectSensor(raster.string());
        const auto second = spm.detectSensor(raster.string());
        EXPECT_EQ(spm.uncachedDetections() - before, 1u);
        EXPECT_EQ(first.detected, second.detected);
        EXPECT_EQ(first.sensorId, second.sensorId);

        createGeoRaster(raster, spec);
        spm.detectSensor(raster.string());
        EXPECT_EQ(spm.uncachedDetections() - before, 2u) << "a rewritten raster is detected again";
    }

    // The clean-up that a tile request starts now and then (one in a thousand) works on a cache
    // that other requests, and other processes sharing it, change at the same time: a folder it
    // listed may be gone when it looks at it, because another clean-up removed it. That is the
    // normal state of the cache, not an error, and it must not fail the tile request that
    // happened to start it.
    TEST(TilerCleanup, cleanupsThatRunTogetherOnAnOldCacheAllFinish)
    {
        TestArea ta(TEST_NAME, true);
        const ScopedProcessConfig cache(json{{"cacheDir", io::toUtf8(ta.getPath("cache"))}}.dump());
        const fs::path tiles = UserProfile::get()->getTilesDir();
        const auto longAgo = fs::file_time_type::clock::now() - std::chrono::hours(24 * 6);

        constexpr int rounds = 12;
        constexpr int threads = 4;
        constexpr int cacheFolders = 40;
        for (int round = 0; round < rounds; round++)
        {
            // What a raster leaves: <hash>/<z>/<x>/<y>.png, all older than the limit
            for (int i = 0; i < cacheFolders; i++)
            {
                const fs::path folder = tiles / ("c" + std::to_string(round) + "-" + std::to_string(i));
                for (const char *level : {"3", "4"})
                    for (const char *column : {"1", "2", "3"})
                    {
                        writeFile(folder / level / column / "1.png", "tile");
                        fs::last_write_time(folder / level / column, longAgo);
                        fs::last_write_time(folder / level, longAgo);
                    }
                fs::last_write_time(folder, longAgo);
            }

            std::atomic<int> failures{0};
            std::string firstFailure;
            std::mutex failureMutex;
            std::atomic<bool> go{false};
            std::vector<std::thread> cleaners;
            for (int t = 0; t < threads; t++)
                cleaners.emplace_back([&] {
                    while (!go.load())
                        std::this_thread::yield();
                    try
                    {
                        TilerHelper::cleanupUserCache();
                    }
                    catch (const std::exception &e)
                    {
                        std::lock_guard<std::mutex> lock(failureMutex);
                        failures++;
                        if (firstFailure.empty())
                            firstFailure = e.what();
                    }
                });
            go = true;
            for (auto &cleaner : cleaners)
                cleaner.join();

            EXPECT_EQ(failures.load(), 0) << "round " << round << ": " << firstFailure;
        }
    }

    // What a clean-up removes when it runs alone: the folders not touched for five days, at any
    // level, and nothing else
    TEST(TilerCleanup, aCleanupRemovesTheFoldersNotTouchedForFiveDaysAtAnyLevel)
    {
        TestArea ta(TEST_NAME, true);
        const ScopedProcessConfig cache(json{{"cacheDir", io::toUtf8(ta.getPath("cache"))}}.dump());
        const fs::path tiles = UserProfile::get()->getTilesDir();
        const auto longAgo = fs::file_time_type::clock::now() - std::chrono::hours(24 * 6);
        const auto recent = fs::file_time_type::clock::now() - std::chrono::hours(24);

        // Old as a whole
        writeFile(tiles / "old" / "3" / "1" / "1.png", "tile");
        for (const fs::path &folder : {fs::path("old") / "3" / "1", fs::path("old") / "3", fs::path("old")})
            fs::last_write_time(tiles / folder, longAgo);
        // Recent, with a level nobody asked for since it was made
        writeFile(tiles / "recent" / "3" / "1" / "1.png", "tile");
        writeFile(tiles / "recent" / "4" / "1" / "1.png", "tile");
        for (const fs::path &folder : {fs::path("recent") / "3" / "1", fs::path("recent") / "3",
                                       fs::path("recent") / "4" / "1", fs::path("recent") / "4"})
            fs::last_write_time(tiles / folder, folder.parent_path().filename() == "3" || folder.filename() == "3" ? longAgo : recent);
        fs::last_write_time(tiles / "recent", recent);
        // A file at the top, and a link to a folder outside that is old
        writeFile(tiles / "loose.txt", "not a folder");
        writeFile(ta.getPath("outside") / "keep.txt", "somebody else's");
        fs::last_write_time(ta.getPath("outside"), longAgo);
#ifndef _WIN32
        fs::create_directory_symlink(ta.getPath("outside"), tiles / "link");
#endif

        TilerHelper::cleanupUserCache();

        EXPECT_FALSE(fs::exists(tiles / "old"));
        EXPECT_TRUE(fs::exists(tiles / "recent" / "4" / "1" / "1.png"));
        EXPECT_FALSE(fs::exists(tiles / "recent" / "3")) << "an old level of a folder that is in use goes";
        EXPECT_TRUE(fs::exists(tiles / "loose.txt"));
        EXPECT_TRUE(fs::exists(ta.getPath("outside") / "keep.txt")) << "a link is not followed";
    }

    TEST(TileEncoderFormats, formats)
    {
        RasterImage image(16, 16, 3, true);
        std::fill(image.color.begin(), image.color.end(), 100);
        std::fill(image.alpha.begin(), image.alpha.end(), 255);
        std::fill(image.alpha.begin(), image.alpha.begin() + 16 * 8, 0); // top half transparent

        for (const auto format : {ImageFormat::Png, ImageFormat::Webp, ImageFormat::Jpeg})
        {
            EncodeOptions options;
            options.format = format;
            uint8_t *buf = nullptr;
            int size = 0;
            TileEncoder::encode(image, options, &buf, &size);
            const auto img = decodeImage(takeBuffer(buf, size));
            EXPECT_EQ(img.width, 16);
            EXPECT_EQ(img.hasAlpha(), format != ImageFormat::Jpeg);
            if (format == ImageFormat::Jpeg)
                EXPECT_GE(img.bands[0][16 * 2 + 8], 250) << "transparent pixels are white in JPEG";
        }

        EXPECT_EQ(parseImageFormat("image/webp"), ImageFormat::Webp);
        EXPECT_EQ(parseImageFormat("JPG"), ImageFormat::Jpeg);
        EXPECT_THROW(parseImageFormat("gif"), InvalidArgsException);
    }
}
