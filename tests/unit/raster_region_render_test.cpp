/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The natural render of a region (renderRasterRegion, the WMS map) as an image shows a raster as
// the map tiles do: its first three data bands (its first, for one or two) stretched to 8 bits
// over the range of their statistics when they are not 8-bit. A 6-band Float32 multispectral
// orthophoto (R, G, B, NIR, RedEdge, alpha) could not be encoded as PNG (at most 4 bands of 8 or
// 16 bits), and the map was transparent; a Float32 DEM was cast to bytes (white above 255 m).

#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"
#include "raster_test_utils.h"

#include "ddb.h"
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

        // The range the tiles stretch the first three bands over
        const auto ds = adoptDataset(GDALOpen(raster.string().c_str(), GA_ReadOnly));
        ASSERT_TRUE(ds);
        const auto [lo, hi] = displayRange(raster.string(), datasetHandle(ds), {1, 2, 3});

        const auto png = render(raster, "image/png");
        ASSERT_EQ(png.bands.size(), 4u) << "RGB + alpha";
        ASSERT_TRUE(png.hasAlpha());
        for (int q = 0; q < 4; q++)
        {
            for (int b = 1; b <= 3; b++)
                EXPECT_NEAR(at(png, b - 1, centreX(q), centreY(q)), stretchToByte(reflectance(b, q), lo, hi), 1)
                    << "band " << b << ", quadrant " << q;
            EXPECT_EQ(at(png, 3, centreX(q), centreY(q)), 255) << "quadrant " << q;
        }
        EXPECT_EQ(at(png, 3, 2, 2), 0) << "the pixels without data stay transparent";

        // JPEG has no alpha: the same colours, white where there is no data
        const auto jpeg = render(raster, "image/jpeg");
        ASSERT_EQ(jpeg.bands.size(), 3u);
        for (int q = 0; q < 4; q++)
            for (int b = 1; b <= 3; b++)
                EXPECT_NEAR(at(jpeg, b - 1, centreX(q), centreY(q)), stretchToByte(reflectance(b, q), lo, hi), 6)
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
        ASSERT_EQ(png.bands.size(), 2u) << "grey + alpha";
        EXPECT_EQ(at(png, 0, centreX(0), centreY(0)), 0);
        EXPECT_NEAR(at(png, 0, centreX(1), centreY(1)), 85, 1);
        EXPECT_NEAR(at(png, 0, centreX(2), centreY(2)), 170, 1);
        EXPECT_EQ(at(png, 0, centreX(3), centreY(3)), 255);
        EXPECT_EQ(at(png, 1, centreX(0), centreY(0)), 255);
    }
}
