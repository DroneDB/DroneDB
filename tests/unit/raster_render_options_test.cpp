/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The options of the region and index renders (DDBRenderRasterRegionEx2, DDBRenderRasterIndexEx): an
// image composited over a background, and the "rgba" format, the pixels of a render instead of an
// encoded image. A WMS host renders a map of one layer in its final format in one call, and reads the
// layers of a map of several as pixels to composite them, with no encode and decode in between.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"
#include "raster_test_utils.h"
#include "synthetic_raster_support.h"

#include "ddb.h"
#include "gdal_inc.h"
#include "gdal_raii.h"
#include "tile_encoder.h"

namespace
{
    constexpr int kWidth = 64;
    constexpr int kHeight = 48;
    constexpr double kPixel = 4.0;

    // The raster plus a margin of 8 pixels outside it on every side, rendered at its resolution
    const double kBbox[4] = {golden::kOriginX - 8 * kPixel, golden::kOriginY - (kHeight + 8) * kPixel,
                             golden::kOriginX + (kWidth + 8) * kPixel, golden::kOriginY + 8 * kPixel};
    constexpr int kOutWidth = kWidth + 16;
    constexpr int kOutHeight = kHeight + 16;

    // The extent of the multispectral raster of the goldens with a margin, for the index
    const double kIndexBbox[4] = {golden::kOriginX - 64.0, golden::kOriginY - golden::kSize * golden::kPixel - 64.0,
                                  golden::kOriginX + golden::kSize * golden::kPixel + 64.0, golden::kOriginY + 64.0};

    constexpr uint32_t kBackground = 0x336699u;

    // A raster in Web Mercator at kPixel of the golden origin
    testutils::GeoRasterSpec spec(int bands, GDALDataType type)
    {
        testutils::GeoRasterSpec spec;
        spec.width = kWidth;
        spec.height = kHeight;
        spec.bands = bands;
        spec.type = type;
        spec.epsg = 3857;
        spec.originX = golden::kOriginX;
        spec.originY = golden::kOriginY;
        spec.pixelSize = kPixel;
        return spec;
    }

    // Smooth RGB, and an alpha band that rises from 0 to 255 over columns 8 to 40: transparent, partial and
    // opaque pixels
    fs::path makeRgba(const fs::path &path)
    {
        auto s = spec(4, GDT_Byte);
        s.alphaBand = true;
        s.alphaValue = [](int x, int y) { return std::clamp((x - 8) * 8 + (y % 3), 0, 255) * 1.0; };
        s.value = [](int band, int x, int y) { return std::min(255.0, 30.0 * band + x * 2.0 + y * 1.5); };
        return testutils::createGeoRaster(path, s);
    }

    // A grey Float32 raster (a DEM) with a nodata patch: stretched to 8 bits with an alpha band
    fs::path makeDem(const fs::path &path)
    {
        auto s = spec(1, GDT_Float32);
        s.value = [](int, int x, int y) { return x >= 10 && x < 20 && y >= 10 && y < 30 ? golden::kNodata : 100.0 + x * 1.5 + y; };
        testutils::createGeoRaster(path, s);
        golden::setNodata(path, golden::kNodata);
        return path;
    }

    // RGB without alpha or nodata: opaque where the raster is
    fs::path makeRgb(const fs::path &path)
    {
        auto s = spec(3, GDT_Byte);
        s.value = [](int band, int x, int y) { return std::min(255.0, x * 3.0 + y * (band + 1.0)); };
        return testutils::createGeoRaster(path, s);
    }

    struct Render
    {
        DDBErr code = DDBERR_NONE;
        std::vector<uint8_t> bytes;
    };

    DDBRenderOptions backgroundOptions(uint32_t background)
    {
        return DDBRenderOptions{sizeof(DDBRenderOptions), DDB_RENDER_OPT_BACKGROUND, background, 0};
    }

    Render region(const fs::path &raster, const char *format, const DDBRenderOptions *options,
                  const std::vector<int> &bands = {})
    {
        Render r;
        uint8_t *buffer = nullptr;
        int size = 0;
        r.code = DDBRenderRasterRegionEx2(raster.string().c_str(), kBbox, "EPSG:3857", nullptr,
                                          bands.empty() ? nullptr : bands.data(), static_cast<int>(bands.size()), kOutWidth,
                                          kOutHeight, format, options, &buffer, &size);
        if (r.code == DDBERR_NONE)
            r.bytes = testutils::takeBuffer(buffer, size);
        return r;
    }

    Render index(const fs::path &raster, const char *format, const DDBRenderOptions *options)
    {
        Render r;
        uint8_t *buffer = nullptr;
        int size = 0;
        r.code = DDBRenderRasterIndexEx(raster.string().c_str(), "NDVI", kIndexBbox, "EPSG:3857", kOutWidth, kOutHeight,
                                        format, options, &buffer, &size);
        if (r.code == DDBERR_NONE)
            r.bytes = testutils::takeBuffer(buffer, size);
        return r;
    }

    // A decoded image as RGBA, as a host decodes it: grey in red, green and blue, alpha 255 without alpha
    std::vector<uint8_t> asRgba(const testutils::DecodedImage &img)
    {
        const size_t pixels = static_cast<size_t>(img.width) * img.height;
        const size_t colors = img.bands.size() - (img.hasAlpha() ? 1 : 0);
        std::vector<uint8_t> rgba(pixels * 4);
        for (size_t i = 0; i < pixels; i++)
        {
            for (size_t c = 0; c < 3; c++)
                rgba[i * 4 + c] = img.bands[colors == 1 ? 0 : c][i];
            rgba[i * 4 + 3] = img.hasAlpha() ? img.alpha()[i] : 255;
        }
        return rgba;
    }

    // Straight-alpha colour c of alpha a composited over an opaque bg, as Skia rounds it
    uint8_t over(unsigned c, unsigned a, unsigned bg)
    {
        return static_cast<uint8_t>((c * a + 127) / 255 + ((bg * (255 - a) + 255) >> 8));
    }

    // The RGB of rgba composited over background
    std::vector<uint8_t> composited(const std::vector<uint8_t> &rgba, uint32_t background)
    {
        const unsigned under[3] = {(background >> 16) & 0xFF, (background >> 8) & 0xFF, background & 0xFF};
        std::vector<uint8_t> rgb;
        for (size_t i = 0; i < rgba.size(); i += 4)
            for (int c = 0; c < 3; c++)
                rgb.push_back(over(rgba[i + c], rgba[i + 3], under[c]));
        return rgb;
    }

    std::vector<uint8_t> rgbOf(const std::vector<uint8_t> &rgba)
    {
        std::vector<uint8_t> rgb;
        for (size_t i = 0; i < rgba.size(); i += 4)
            rgb.insert(rgb.end(), rgba.begin() + static_cast<long>(i), rgba.begin() + static_cast<long>(i) + 3);
        return rgb;
    }

    // The PNG image encoded in format with the options of the map renders
    std::vector<uint8_t> encodedAs(const std::vector<uint8_t> &png, ddb::ImageFormat format)
    {
        const ddb::VsiMemFile in = ddb::VsiMemFile::unique("test-png-", ".png");
        const ddb::VsiMemFile out = ddb::VsiMemFile::unique("test-enc-", std::string(".") + ddb::imageFormatExtension(format));
        VSIFCloseL(VSIFileFromMemBuffer(in.c_str(), const_cast<uint8_t *>(png.data()), png.size(), FALSE));
        const auto source = ddb::adoptDataset(GDALOpen(in.c_str(), GA_ReadOnly));
        const CPLStringList options = ddb::cslFromStrings(ddb::imageCreationOptions({format}));
        auto encoded = ddb::adoptDataset(GDALCreateCopy(GDALGetDriverByName(format == ddb::ImageFormat::Jpeg ? "JPEG" : "WEBP"),
                                                        out.c_str(), ddb::datasetHandle(source), FALSE, options.List(),
                                                        nullptr, nullptr));
        ddb::closeOutputOrThrow(std::move(encoded), out.path());
        int size = 0;
        auto bytes = ddb::stealMemFileBuffer(out.path(), size);
        return std::vector<uint8_t>(bytes.get(), bytes.get() + size);
    }

    class RenderOptions : public ::testing::Test
    {
    protected:
        void SetUp() override { DDBRegisterProcess(false); }

        // The renders of the tests: name, then the call with a format and options
        using Call = std::function<Render(const char *, const DDBRenderOptions *)>;
        std::vector<std::pair<std::string, Call>> renders(TestArea &ta)
        {
            const fs::path rgba = makeRgba(ta.getPath("rgba.tif"));
            const fs::path dem = makeDem(ta.getPath("dem.tif"));
            const fs::path rgb = makeRgb(ta.getPath("rgb.tif"));
            const fs::path ms = golden::makeMultispectral(ta.getPath("ms.tif"));
            return {
                {"rgba", [=](const char *f, const DDBRenderOptions *o) { return region(rgba, f, o); }},
                {"dem", [=](const char *f, const DDBRenderOptions *o) { return region(dem, f, o); }},
                {"rgb", [=](const char *f, const DDBRenderOptions *o) { return region(rgb, f, o); }},
                {"ndvi", [=](const char *f, const DDBRenderOptions *o) { return index(ms, f, o); }},
            };
        }
    };

    // The rounding of the composite is that of Skia's SrcOver of a straight-alpha pixel over an opaque
    // one (values Skia gives)
    TEST(RenderBackground, theCompositeRoundsAsSkia)
    {
        const unsigned skia[][4] = {{200, 128, 255, 227}, {10, 1, 200, 199}, {255, 254, 0, 254},
                                    {0, 127, 255, 128},   {77, 200, 51, 71}, {128, 128, 128, 128},
                                    {255, 1, 0, 1},       {1, 255, 254, 1},  {33, 64, 153, 123}};
        for (const auto &v : skia)
            EXPECT_EQ(over(v[0], v[1], v[2]), v[3]) << v[0] << " " << v[1] << " " << v[2];
    }

    // rgba is what the PNG of the same render decodes to: grey as RGB, the alpha of the render
    TEST_F(RenderOptions, rgbaIsWhatThePngDecodesTo)
    {
        TestArea ta(TEST_NAME, true);
        for (const auto &[name, render] : renders(ta))
        {
            SCOPED_TRACE(name);
            const Render raw = render("rgba", nullptr);
            const Render png = render("image/png", nullptr);
            ASSERT_EQ(raw.code, DDBERR_NONE) << DDBGetLastError();
            ASSERT_EQ(png.code, DDBERR_NONE) << DDBGetLastError();
            ASSERT_EQ(raw.bytes.size(), static_cast<size_t>(kOutWidth) * kOutHeight * 4);
            EXPECT_EQ(raw.bytes, asRgba(testutils::decodeImage(png.bytes)));
        }
    }

    // With a background, the PNG and the rgba are the render composited over it, pixel for pixel, with no
    // transparency left; JPEG and WebP are that image encoded once
    TEST_F(RenderOptions, aBackgroundIsTheRenderCompositedOverIt)
    {
        TestArea ta(TEST_NAME, true);
        const DDBRenderOptions options = backgroundOptions(kBackground);
        for (const auto &[name, render] : renders(ta))
        {
            SCOPED_TRACE(name);
            const Render raw = render("rgba", nullptr);
            ASSERT_EQ(raw.code, DDBERR_NONE) << DDBGetLastError();
            const std::vector<uint8_t> expected = composited(raw.bytes, kBackground);
            size_t transparent = 0, partial = 0;
            for (size_t i = 3; i < raw.bytes.size(); i += 4)
            {
                transparent += raw.bytes[i] == 0;
                partial += raw.bytes[i] > 0 && raw.bytes[i] < 255;
            }
            EXPECT_GT(transparent, 0u) << "the margin is outside the raster";
            if (name == "rgba")
            {
                EXPECT_GT(partial, 1000u);
            }

            const Render png = render("image/png", &options);
            ASSERT_EQ(png.code, DDBERR_NONE) << DDBGetLastError();
            const auto image = testutils::decodeImage(png.bytes);
            EXPECT_EQ(image.bands.size(), 3u) << "no alpha";
            EXPECT_EQ(rgbOf(asRgba(image)), expected);

            const Render rgba = render("rgba", &options);
            ASSERT_EQ(rgba.code, DDBERR_NONE) << DDBGetLastError();
            EXPECT_EQ(rgbOf(rgba.bytes), expected);
            for (size_t i = 3; i < rgba.bytes.size(); i += 4)
                ASSERT_EQ(rgba.bytes[i], 255);

            for (const auto format : {ddb::ImageFormat::Jpeg, ddb::ImageFormat::Webp})
            {
                const std::string mime = std::string("image/") + (format == ddb::ImageFormat::Jpeg ? "jpeg" : "webp");
                SCOPED_TRACE(mime);
                const Render encoded = render(mime.c_str(), &options);
                ASSERT_EQ(encoded.code, DDBERR_NONE) << DDBGetLastError();
                EXPECT_EQ(encoded.bytes, encodedAs(png.bytes, format));
                EXPECT_EQ(testutils::decodeImage(encoded.bytes).bands.size(), 3u);
            }
        }
    }

    // Without options the renders are those of DDBRenderRasterRegionEx and DDBRenderRasterIndex, byte for
    // byte, in every format
    TEST_F(RenderOptions, withoutOptionsTheRenderIsTheOneOfTheOlderExport)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path rgba = makeRgba(ta.getPath("rgba.tif"));
        const fs::path ms = golden::makeMultispectral(ta.getPath("ms.tif"));
        for (const char *format : {"image/png", "image/jpeg", "image/webp", "image/tiff", "rgba"})
        {
            SCOPED_TRACE(format);
            uint8_t *buffer = nullptr;
            int size = 0;
            ASSERT_EQ(DDBRenderRasterRegionEx(rgba.string().c_str(), kBbox, "EPSG:3857", nullptr, nullptr, 0, kOutWidth,
                                              kOutHeight, format, &buffer, &size),
                      DDBERR_NONE);
            EXPECT_EQ(region(rgba, format, nullptr).bytes, testutils::takeBuffer(buffer, size));
            ASSERT_EQ(DDBRenderRasterIndex(ms.string().c_str(), "NDVI", kIndexBbox, "EPSG:3857", kOutWidth, kOutHeight,
                                           format, &buffer, &size),
                      DDBERR_NONE);
            EXPECT_EQ(index(ms, format, nullptr).bytes, testutils::takeBuffer(buffer, size));
        }
    }

    // A JPEG has no alpha: one of a raster with an alpha band is RGB over white (the alpha band of the
    // warp made it a four-band JPEG, read as CMYK), and the WebP of a grey render is RGB with the alpha of
    // its PNG (the encoder refused two bands)
    TEST_F(RenderOptions, aJpegIsRgbOverWhiteAndAGreyWebpIsRgb)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path rgba = makeRgba(ta.getPath("rgba.tif"));
        const Render jpeg = region(rgba, "image/jpeg", nullptr);
        ASSERT_EQ(jpeg.code, DDBERR_NONE) << DDBGetLastError();
        const auto decoded = testutils::decodeImage(jpeg.bytes);
        ASSERT_EQ(decoded.bands.size(), 3u);
        // The rendering of JPEG without a background is the one it had (the warp writes white where the
        // raster has no data): the image is that over white, within the compression of smooth bands
        const std::vector<uint8_t> overWhite = composited(region(rgba, "rgba", nullptr).bytes, 0xFFFFFFu);
        const std::vector<uint8_t> rgb = rgbOf(asRgba(decoded));
        double sum = 0;
        for (size_t i = 0; i < rgb.size(); i++)
            sum += std::abs(static_cast<int>(rgb[i]) - static_cast<int>(overWhite[i]));
        EXPECT_LT(sum / static_cast<double>(rgb.size()), 2.0);
        for (int c = 0; c < 3; c++)
            EXPECT_GE(decoded.bands[c][0], 250) << "white outside the raster";

        const fs::path dem = makeDem(ta.getPath("dem.tif"));
        const Render webp = region(dem, "image/webp", nullptr);
        ASSERT_EQ(webp.code, DDBERR_NONE) << DDBGetLastError();
        const auto grey = testutils::decodeImage(webp.bytes);
        EXPECT_EQ(grey.bands.size(), 4u);
        EXPECT_EQ(grey.alpha(), testutils::decodeImage(region(dem, "image/png", nullptr).bytes).alpha());
    }

    // A band selection keeps its bands: rgba of a band with a colour table is its colours, as its PNG (a
    // palette) decodes, and rgba refuses what an image cannot hold (two bands, 16 bits)
    TEST_F(RenderOptions, rgbaOfABandSelectionIsItsImage)
    {
        TestArea ta(TEST_NAME, true);
        auto s = spec(1, GDT_Byte);
        s.value = [](int, int x, int) { return static_cast<double>(x % 4); };
        const fs::path palette = testutils::createGeoRaster(ta.getPath("palette.tif"), s);
        {
            GDALDatasetH ds = GDALOpen(palette.string().c_str(), GA_Update);
            ASSERT_NE(ds, nullptr);
            GDALColorTableH table = GDALCreateColorTable(GPI_RGB);
            for (int i = 0; i < 4; i++)
            {
                const GDALColorEntry entry{static_cast<short>(60 * i), static_cast<short>(255 - 50 * i), 7, 255};
                GDALSetColorEntry(table, i, &entry);
            }
            GDALSetRasterColorTable(GDALGetRasterBand(ds, 1), table);
            GDALDestroyColorTable(table);
            GDALClose(ds);
        }
        const Render raw = region(palette, "rgba", nullptr, {1});
        ASSERT_EQ(raw.code, DDBERR_NONE) << DDBGetLastError();
        // The raster starts at the 9th pixel of the render
        for (int x = 0; x < 4; x++)
        {
            const uint8_t *pixel = &raw.bytes[(static_cast<size_t>(8) * kOutWidth + 8 + x) * 4];
            EXPECT_EQ(std::vector<uint8_t>(pixel, pixel + 4),
                      (std::vector<uint8_t>{static_cast<uint8_t>(60 * x), static_cast<uint8_t>(255 - 50 * x), 7, 255}))
                << "index " << x;
        }

        const fs::path rgb = makeRgb(ta.getPath("rgb.tif"));
        EXPECT_EQ(region(rgb, "rgba", nullptr, {1, 2}).code, DDBERR_INVALIDARGS);
        const fs::path u16 = golden::makeRaster(ta.getPath("u16.tif"), 3, GDT_UInt16, false, std::nullopt);
        EXPECT_EQ(region(u16, "rgba", nullptr, {1, 2, 3}).code, DDBERR_INVALIDARGS);
    }

    // Options that this library does not know or cannot apply are refused
    TEST_F(RenderOptions, invalidOptionsAreRefused)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path rgba = makeRgba(ta.getPath("rgba.tif"));
        const fs::path ms = golden::makeMultispectral(ta.getPath("ms.tif"));
        const auto refused = [&](const DDBRenderOptions &options, const char *format = "image/png") {
            EXPECT_EQ(region(rgba, format, &options).code, DDBERR_INVALIDARGS);
            EXPECT_EQ(index(ms, format, &options).code, DDBERR_INVALIDARGS);
        };
        refused({8, DDB_RENDER_OPT_BACKGROUND, 0, 0});
        refused({sizeof(DDBRenderOptions), 0x2u, 0, 0});
        refused({sizeof(DDBRenderOptions), DDB_RENDER_OPT_BACKGROUND, 0x1000000u, 0});
        refused({sizeof(DDBRenderOptions), DDB_RENDER_OPT_BACKGROUND, 0, 1});
        refused(backgroundOptions(0xFFFFFFu), "image/tiff");
        refused(backgroundOptions(0xFFFFFFu), "image/bmp");

        // A larger struct of a later version is taken when its unknown bytes are zero
        struct Later
        {
            DDBRenderOptions options;
            uint64_t unknown;
        } later{backgroundOptions(kBackground), 0};
        later.options.structSize = sizeof(Later);
        const DDBRenderOptions options = backgroundOptions(kBackground);
        EXPECT_EQ(region(rgba, "image/png", &later.options).bytes, region(rgba, "image/png", &options).bytes);
        later.unknown = 1;
        EXPECT_EQ(region(rgba, "image/png", &later.options).code, DDBERR_INVALIDARGS);
    }
}
