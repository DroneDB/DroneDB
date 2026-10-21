/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DDB-05: raster tiles reuse a prepared tiler, compute statistics once and encode
// through TileEncoder

#include <algorithm>
#include <fstream>
#include <thread>

#include "ddb.h"
#include "gdaltiler.h"
#include "raster_cache.h"
#include "raster_stats.h"
#include "raster_test_utils.h"
#include "sensorprofile.h"
#include "test.h"
#include "testarea.h"
#include "tile_encoder.h"
#include "tilerhelper.h"

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
        GDALTiler t(raster.string(), "", 256);
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
        TilerHelper::getTile(raster, t.z, t.x, t.y, 256, false, false, "", format, &buf, &size);
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

    TEST(DDB05_Tiler, cacheHitSkipsOpen)
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

    TEST(DDB05_Tiler, statsComputedOnce)
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

    TEST(DDB05_Tiler, sidecarUsed)
    {
        TestArea ta(TEST_NAME, true);
        const auto raster = createGeoRaster(ta.getPath("float.tif"), floatRamp(0, 500));
        // The COG build writes this sidecar; here it claims a range twice the data's
        std::ofstream(raster.string() + ".stats.json")
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

    TEST(DDB05_Tiler, concurrentTilesSameRaster)
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

    TEST(DDB05_Tiler, evictOnRebuild)
    {
        TestArea ta(TEST_NAME, true);
        auto spec = rgbGradient();
        spec.value = [](int, int, int) { return 10.0; };
        const auto raster = createGeoRaster(ta.getPath("rebuilt.tif"), spec);
        const auto tile = fullTile(raster);
        EXPECT_EQ(maxOfBand(decodeImage(memoryTile(raster, tile)), 0), 10);

        // Rebuilt in place, as a COG build replaces its output
        spec.value = [](int, int, int) { return 200.0; };
        createGeoRaster(raster, spec);
        EXPECT_EQ(maxOfBand(decodeImage(memoryTile(raster, tile)), 0), 200);
    }

    TEST(DDB05_Tiler, releaseClosesIdleRasters)
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

    TEST(DDB05_Tiler, opaqueTileHasNoAlpha)
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

    TEST(DDB05_Tiler, webpAndJpegTiles)
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

    TEST(DDB05_Tiler, blankTilesShareTheirBytes)
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

    TEST(DDB05_Tiler, sensorDetectionIsMemoised)
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

    TEST(DDB05_TileEncoder, formats)
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
