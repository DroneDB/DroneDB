/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The natural render of a region (renderRasterRegion, the WMS map) as an image shows a raster as
// the map tiles do: its first three data bands (its first, for one or two) stretched to 8 bits
// over the range of their statistics when they are not 8-bit. A 6-band Float32 multispectral
// orthophoto (R, G, B, NIR, RedEdge, alpha) could not be encoded as PNG (at most 4 bands of 8 or
// 16 bits), and the map was transparent; a Float32 DEM was cast to bytes (white above 255 m).

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"
#include "raster_test_utils.h"

#include "ddb.h"
#include "ddbt/ddbt.h"
#include "raster_region.h"
#include "gdal_raii.h"
#include "raster_stats.h"
#include "vis_render.h"

namespace
{
    using namespace ddb;

    class RasterRegionRender : public ::testing::Test
    {
    protected:
        void SetUp() override { DDBRegisterProcess(false); }
    };

    constexpr int kSize = 64;
    constexpr double kOriginX = 1113000.0, kOriginY = 5621000.0, kPixel = 4.0;
    // The extent of the raster, so the warp of the render is an identity
    const double kBbox[4] = {kOriginX, kOriginY - kSize * kPixel, kOriginX + kSize * kPixel, kOriginY};

    // The quadrant of a pixel: 0 top left, 1 top right, 2 bottom left, 3 bottom right
    int quadrant(int x, int y) { return (y >= kSize / 2 ? 2 : 0) + (x >= kSize / 2 ? 1 : 0); }

    // The top left 8 x 8 pixels have no data (alpha 0)
    bool transparent(int x, int y) { return x < 8 && y < 8; }

    // Reflectance of band b (1-based) in quadrant q
    double reflectance(int b, int q) { return 0.02 + 0.05 * b + 0.1 * q; }

    fs::path multispectral(const fs::path &path)
    {
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = kSize;
        spec.bands = 6;
        spec.type = GDT_Float32;
        spec.originX = kOriginX;
        spec.originY = kOriginY;
        spec.pixelSize = kPixel;
        spec.alphaBand = true;
        spec.alphaValue = [](int x, int y) { return transparent(x, y) ? 0.0 : 255.0; };
        spec.bandNames = {"Red", "Green", "Blue", "NIR", "Rededge"};
        spec.value = [](int b, int x, int y) { return reflectance(b, quadrant(x, y)); };
        return testutils::createGeoRaster(path, spec);
    }

    testutils::DecodedImage render(const fs::path &raster, const std::string &format)
    {
        uint8_t *buffer = nullptr;
        int size = 0;
        renderRasterRegion(testutils::regionRequest(raster.string(), kBbox, "EPSG:3857", kSize, kSize, format), {},
                           &buffer, &size);
        return testutils::decodeImage(testutils::takeBuffer(buffer, size));
    }

    uint8_t at(const testutils::DecodedImage &image, int band, int x, int y)
    {
        return image.bands[static_cast<size_t>(band)][static_cast<size_t>(y) * image.width + x];
    }

    // The centre pixel of a quadrant
    int centreX(int q) { return (q % 2) * kSize / 2 + kSize / 4; }
    int centreY(int q) { return (q / 2) * kSize / 2 + kSize / 4; }

    TEST_F(RasterRegionRender, aFloatMultispectralRasterShowsItsFirstThreeBandsStretchedAsTheTiles)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path raster = multispectral(ta.getPath("multispectral.tif"));

        // The range the tiles stretch each of the first three bands over: its own (D-041, FR-RAS-21)
        const auto ds = adoptDataset(GDALOpen(raster.string().c_str(), GA_ReadOnly));
        ASSERT_TRUE(ds);
        const auto ranges = defaultStretch(raster.string(), datasetHandle(ds), {1, 2, 3});
        ASSERT_EQ(ranges.size(), 3u);

        const auto png = render(raster, "image/png");
        ASSERT_EQ(png.bands.size(), 4u) << "RGB + alpha";
        ASSERT_TRUE(png.hasAlpha());
        for (int q = 0; q < 4; q++)
        {
            for (int b = 1; b <= 3; b++)
                EXPECT_NEAR(at(png, b - 1, centreX(q), centreY(q)), stretchToByte(reflectance(b, q), ranges[b - 1].first, ranges[b - 1].second), 1)
                    << "band " << b << ", quadrant " << q;
            EXPECT_EQ(at(png, 3, centreX(q), centreY(q)), 255) << "quadrant " << q;
        }
        EXPECT_EQ(at(png, 3, 2, 2), 0) << "the pixels without data stay transparent";

        // JPEG has no alpha: the same colours, white where there is no data
        const auto jpeg = render(raster, "image/jpeg");
        ASSERT_EQ(jpeg.bands.size(), 3u);
        for (int q = 0; q < 4; q++)
            for (int b = 1; b <= 3; b++)
                EXPECT_NEAR(at(jpeg, b - 1, centreX(q), centreY(q)), stretchToByte(reflectance(b, q), ranges[b - 1].first, ranges[b - 1].second), 6)
                    << "band " << b << ", quadrant " << q;
        for (int b = 0; b < 3; b++)
            EXPECT_GE(at(jpeg, b, 2, 2), 245) << "band " << b + 1;
    }

    // A DEM 300 to 360 m high is grey from black to white, not cast to bytes (white everywhere)
    TEST_F(RasterRegionRender, aFloatDemShowsAsGreyStretchedOverItsRange)
    {
        TestArea ta(TEST_NAME, true);
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = kSize;
        spec.bands = 1;
        spec.type = GDT_Float32;
        spec.originX = kOriginX;
        spec.originY = kOriginY;
        spec.pixelSize = kPixel;
        spec.value = [](int, int x, int y) { return 300.0 + 20.0 * quadrant(x, y); };
        const fs::path dem = testutils::createGeoRaster(ta.getPath("dem.tif"), spec);

        const auto png = render(dem, "image/png");
        ASSERT_EQ(png.bands.size(), 1u) << "grey, without the alpha that is 255 everywhere";
        EXPECT_EQ(at(png, 0, centreX(0), centreY(0)), 0);
        EXPECT_NEAR(at(png, 0, centreX(1), centreY(1)), 85, 1);
        EXPECT_NEAR(at(png, 0, centreX(2), centreY(2)), 170, 1);
        EXPECT_EQ(at(png, 0, centreX(3), centreY(3)), 255);
        EXPECT_EQ(png.alpha()[static_cast<size_t>(centreY(0)) * png.width + centreX(0)], 255);
    }

    // The types the normalized copy of a raster keeps that GDAL rendered and ddb-tilers reads too (Float16, Int64): a
    // DEM of them is grey stretched over its range, and its GeoTIFF keeps its type and values
    TEST_F(RasterRegionRender, aFloat16OrInt64DemRendersAsAnyDem)
    {
        TestArea ta(TEST_NAME, true);
        for (const GDALDataType type : {GDT_Float16, GDT_Int64})
        {
            SCOPED_TRACE(GDALGetDataTypeName(type));
            testutils::GeoRasterSpec spec;
            spec.width = spec.height = kSize;
            spec.bands = 1;
            spec.type = type;
            spec.originX = kOriginX;
            spec.originY = kOriginY;
            spec.pixelSize = kPixel;
            spec.value = [](int, int x, int y) { return 300.0 + 20.0 * quadrant(x, y); };
            const fs::path dem =
                testutils::createGeoRaster(ta.getPath(std::string(GDALGetDataTypeName(type)) + ".tif"), spec);

            const auto png = render(dem, "image/png");
            ASSERT_EQ(png.bands.size(), 1u);
            EXPECT_EQ(at(png, 0, centreX(0), centreY(0)), 0);
            EXPECT_NEAR(at(png, 0, centreX(1), centreY(1)), 85, 1);
            EXPECT_NEAR(at(png, 0, centreX(2), centreY(2)), 170, 1);
            EXPECT_EQ(at(png, 0, centreX(3), centreY(3)), 255);

            uint8_t *buffer = nullptr;
            int size = 0;
            renderRasterRegion(testutils::regionRequest(dem.string(), kBbox, "EPSG:3857", kSize, kSize, "image/tiff"), {},
                               &buffer, &size);
            const auto bytes = testutils::takeBuffer(buffer, size);
            const fs::path tif = ta.getPath(std::string(GDALGetDataTypeName(type)) + "-render.tif");
            std::ofstream(tif, std::ios::binary).write(reinterpret_cast<const char *>(bytes.data()),
                                                       static_cast<std::streamsize>(bytes.size()));
            const auto ds = adoptDataset(GDALOpen(tif.string().c_str(), GA_ReadOnly));
            ASSERT_TRUE(ds);
            GDALRasterBandH band = GDALGetRasterBand(datasetHandle(ds), 1);
            EXPECT_EQ(GDALGetRasterDataType(band), type);
            for (int q = 0; q < 4; q++)
            {
                double v = 0;
                ASSERT_EQ(GDALRasterIO(band, GF_Read, centreX(q), centreY(q), 1, 1, &v, 1, 1, GDT_Float64, 0, 0), CE_None);
                EXPECT_EQ(v, 300.0 + 20.0 * q) << "quadrant " << q;
            }
        }
    }

    // A raster of classes with a colour table (a land cover, a classified orthophoto): every format shows
    // the colours of its classes, and the warp of the render does not blend two classes into a third
    // (2.6 showed the indices as grey in PNG and JPEG, and resampled them bilinearly)
    TEST_F(RasterRegionRender, aRasterWithAColourTableShowsTheColoursOfItsClasses)
    {
        TestArea ta(TEST_NAME, true);
        const std::array<std::array<uint8_t, 4>, 4> colours = {
            {{230, 25, 75, 255}, {60, 180, 75, 255}, {0, 130, 200, 255}, {255, 225, 25, 255}}};
        const fs::path raster = ta.getPath("classes.tif");
        {
            auto ds = adoptDataset(GDALCreate(GDALGetDriverByName("GTiff"), raster.string().c_str(), kSize, kSize, 1,
                                              GDT_Byte, nullptr));
            ASSERT_TRUE(ds);
            const double transform[6] = {kOriginX, kPixel, 0, kOriginY, 0, -kPixel};
            GDALSetGeoTransform(datasetHandle(ds), const_cast<double *>(transform));
            OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
            OSRImportFromEPSG(srs, 3857);
            GDALSetSpatialRef(datasetHandle(ds), srs);
            OSRDestroySpatialReference(srs);
            GDALColorTableH table = GDALCreateColorTable(GPI_RGB);
            for (int i = 0; i < 4; i++)
            {
                const GDALColorEntry entry{colours[i][0], colours[i][1], colours[i][2], colours[i][3]};
                GDALSetColorEntry(table, i, &entry);
            }
            GDALSetRasterColorTable(GDALGetRasterBand(datasetHandle(ds), 1), table);
            GDALDestroyColorTable(table);
            std::vector<uint8_t> classes(static_cast<size_t>(kSize) * kSize);
            for (int y = 0; y < kSize; y++)
                for (int x = 0; x < kSize; x++)
                    classes[static_cast<size_t>(y) * kSize + x] = static_cast<uint8_t>(quadrant(x, y));
            ASSERT_EQ(GDALRasterIO(GDALGetRasterBand(datasetHandle(ds), 1), GF_Write, 0, 0, kSize, kSize,
                                   classes.data(), kSize, kSize, GDT_Byte, 0, 0),
                      CE_None);
        }

        // A window a third of a pixel off the grid of the raster, and larger than it: every pixel of the
        // render is resampled, and the pixels outside the raster are transparent
        const double shift = kPixel / 3;
        const double bbox[4] = {kBbox[0] - 4 * kPixel + shift, kBbox[1] - 4 * kPixel + shift,
                                kBbox[2] + 4 * kPixel + shift, kBbox[3] + 4 * kPixel + shift};
        const int size = kSize + 8;
        const auto renderAs = [&](const std::string &format) {
            uint8_t *buffer = nullptr;
            int bytes = 0;
            renderRasterRegion(testutils::regionRequest(raster.string(), bbox, "EPSG:3857", size, size, format), {},
                               &buffer, &bytes);
            return testutils::takeBuffer(buffer, bytes);
        };

        const std::vector<uint8_t> rgba = renderAs(kRgbaPixelsFormat);
        ASSERT_EQ(rgba.size(), static_cast<size_t>(size) * size * 4);
        size_t opaque = 0;
        for (size_t p = 0; p < rgba.size(); p += 4)
        {
            if (rgba[p + 3] == 0)
                continue;
            opaque++;
            const std::array<uint8_t, 4> pixel = {rgba[p], rgba[p + 1], rgba[p + 2], rgba[p + 3]};
            ASSERT_NE(std::find(colours.begin(), colours.end(), pixel), colours.end())
                << "pixel " << p / 4 << " is " << int(pixel[0]) << "," << int(pixel[1]) << "," << int(pixel[2]) << ","
                << int(pixel[3]) << ": not the colour of a class";
        }
        EXPECT_GT(opaque, static_cast<size_t>(kSize - 1) * (kSize - 1));
        // The centre of each quadrant has the colour of its class
        for (int q = 0; q < 4; q++)
        {
            const size_t p = (static_cast<size_t>(centreY(q) + 4) * size + centreX(q) + 4) * 4;
            for (int c = 0; c < 4; c++)
                EXPECT_EQ(rgba[p + c], colours[q][c]) << "quadrant " << q << ", channel " << c;
        }

        // The PNG decodes to those pixels; JPEG (over white) and WebP are close to them
        const auto png = testutils::decodeImage(renderAs("image/png"));
        for (const char *format : {"image/jpeg", "image/webp"})
        {
            SCOPED_TRACE(format);
            const auto lossy = testutils::decodeImage(renderAs(format));
            ASSERT_GE(lossy.bands.size(), 3u);
            for (int q = 0; q < 4; q++)
            {
                for (int c = 0; c < 3; c++)
                {
                    EXPECT_NEAR(at(lossy, c, centreX(q) + 4, centreY(q) + 4), colours[q][c], 6)
                        << "quadrant " << q << ", channel " << c;
                }
            }
        }
        ASSERT_GE(png.bands.size(), 3u);
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++)
            {
                const size_t i = static_cast<size_t>(y) * size + x;
                ASSERT_EQ(png.alpha()[i], rgba[i * 4 + 3]) << x << "," << y;
                if (rgba[i * 4 + 3] > 0)
                {
                    for (int c = 0; c < 3; c++)
                    {
                        ASSERT_EQ(at(png, c, x, y), rgba[i * 4 + c]) << x << "," << y << ", channel " << c;
                    }
                }
            }
    }

    // Renders of one raster made by several threads at once are those made one at a time, with a block
    // cache too small to keep its blocks: the warps then wait for blocks other threads decode, and a
    // waiting thread must not start another tile in the middle of its own (the warp keeps per-thread
    // scratch through a tile)
    TEST_F(RasterRegionRender, concurrentRendersOfOneRasterAreThoseMadeOneAtATime)
    {
        TestArea ta(TEST_NAME, true);
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = 2048;
        spec.originX = kOriginX;
        spec.originY = kOriginY;
        spec.pixelSize = 1.0;
        spec.value = [](int b, int x, int y) { return static_cast<double>((x * 7 + y * 13 + b * 61) % 256); };
        const fs::path raster = testutils::createGeoRaster(ta.getPath("big.tif"), spec);

        // 36 windows of 256 x 256 pixels at 1, 2 and 3 source pixels per output pixel
        std::vector<std::array<double, 4>> boxes;
        for (int i = 0; i < 6; i++)
            for (int j = 0; j < 6; j++)
            {
                const double size = 256.0 * (1 + (i + j) % 3);
                const double x0 = kOriginX + (2048 - size) * i / 5, y1 = kOriginY - (2048 - size) * j / 5;
                boxes.push_back({x0, y1 - size, x0 + size, y1});
            }
        const auto renderBox = [&](const std::array<double, 4> &box) {
            uint8_t *buffer = nullptr;
            int size = 0;
            renderRasterRegion(testutils::regionRequest(raster.string(), box.data(), "EPSG:3857", 256, 256, "rgba"), {},
                               &buffer, &size);
            return testutils::takeBuffer(buffer, size);
        };
        std::vector<std::vector<uint8_t>> want;
        for (const auto &box : boxes)
            want.push_back(renderBox(box));

        ASSERT_EQ(ddbtConfigure("{\"blockCacheMb\": 1}"), DDBT_ERR_NONE);
        std::atomic<int> differ{0}, failed{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 12; t++)
            threads.emplace_back([&, t] {
                for (int rep = 0; rep < 4; rep++)
                    for (size_t i = static_cast<size_t>(t) % 3; i < boxes.size(); i += 3)
                    {
                        try
                        {
                            if (renderBox(boxes[i]) != want[i])
                                differ++;
                        }
                        catch (const std::exception &)
                        {
                            failed++;
                        }
                    }
            });
        for (auto &thread : threads)
            thread.join();
        ASSERT_EQ(ddbtConfigure("{\"blockCacheMb\": 1024}"), DDBT_ERR_NONE);
        EXPECT_EQ(differ.load(), 0);
        EXPECT_EQ(failed.load(), 0);
    }
}
