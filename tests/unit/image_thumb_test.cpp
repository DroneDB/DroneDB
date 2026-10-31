/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Image thumbnails stretch only non-Byte data and keep transparency

#include <algorithm>
#include <numeric>

#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"
#include "thumbs.h"

namespace
{
    using namespace ddb;
    using namespace testutils;

    DecodedImage thumbnail(const fs::path &raster, int size = 128)
    {
        uint8_t *buf = nullptr;
        int bufSize = 0;
        generateThumb(raster, size, "", true, &buf, &bufSize);
        return decodeImage(takeBuffer(buf, bufSize));
    }

    std::pair<int, int> range(const std::vector<uint8_t> &plane)
    {
        const auto [mn, mx] = std::minmax_element(plane.begin(), plane.end());
        return {*mn, *mx};
    }

    TEST(ImageThumbnail, byteRgbNotStretched)
    {
        TestArea ta(TEST_NAME, true);
        GeoRasterSpec spec;
        spec.width = spec.height = 256;
        spec.value = [](int, int x, int) { return 50.0 + 50.0 * x / 255.0; }; // 50..100
        const auto raster = createGeoRaster(ta.getPath("rgb.tif"), spec);

        const auto img = thumbnail(raster);
        ASSERT_EQ(img.bands.size(), 3u);
        for (int b = 0; b < 3; b++)
        {
            const auto [mn, mx] = range(img.bands[b]);
            EXPECT_GE(mn, 47) << "band " << b;
            EXPECT_LE(mx, 103) << "band " << b;
        }
    }

    TEST(ImageThumbnail, floatDsmStillStretched)
    {
        TestArea ta(TEST_NAME, true);
        GeoRasterSpec spec;
        spec.width = spec.height = 256;
        spec.bands = 1;
        spec.type = GDT_Float32;
        spec.value = [](int, int x, int) { return 100.0 + 100.0 * x / 255.0; }; // 100..200 m
        const auto raster = createGeoRaster(ta.getPath("dsm.tif"), spec);

        const auto img = thumbnail(raster);
        const auto [mn, mx] = range(img.bands[0]);
        EXPECT_LE(mn, 5);
        EXPECT_GE(mx, 250);
    }

    TEST(ImageThumbnail, alphaPreserved)
    {
        TestArea ta(TEST_NAME, true);
        GeoRasterSpec spec;
        spec.width = spec.height = 256;
        spec.bands = 4;
        spec.alphaBand = true;
        spec.value = [](int, int x, int) { return static_cast<double>(x); };
        auto raster = createGeoRaster(ta.getPath("rgba.tif"), spec);

        // Transparent top-left quarter
        GDALDatasetH ds = GDALOpen(raster.string().c_str(), GA_Update);
        std::vector<uint8_t> zeros(128 * 128, 0);
        ASSERT_EQ(GDALRasterIO(GDALGetRasterBand(ds, 4), GF_Write, 0, 0, 128, 128, zeros.data(), 128, 128, GDT_Byte,
                               0, 0),
                  CE_None);
        GDALClose(ds);

        const auto img = thumbnail(raster);
        ASSERT_EQ(img.bands.size(), 4u);
        ASSERT_TRUE(img.hasAlpha());
        EXPECT_EQ(img.alpha()[0], 0) << "transparent corner";
        EXPECT_EQ(img.alpha().back(), 255) << "opaque corner";
    }
}
