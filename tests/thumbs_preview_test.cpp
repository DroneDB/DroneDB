/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DDB-10: JPEG thumbnails decode an embedded MPF preview of the same framing when it is
// large enough

#include <fstream>

#include "embedded_preview.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"
#include "thumbs.h"

namespace
{
    using namespace ddb;
    using namespace testutils;

    struct Rgb
    {
        uint8_t r, g, b;
    };

    const Rgb Red{220, 20, 20};
    const Rgb Blue{20, 20, 220};

    // A width x height JPEG of one colour
    std::vector<uint8_t> solidJpeg(int width, int height, Rgb colour)
    {
        GDALDatasetH mem = GDALCreate(GDALGetDriverByName("MEM"), "", width, height, 3, GDT_Byte, nullptr);
        const uint8_t values[3] = {colour.r, colour.g, colour.b};
        for (int b = 0; b < 3; b++)
            GDALFillRaster(GDALGetRasterBand(mem, b + 1), values[b], 0);
        const std::string vsi = "/vsimem/thumbs_preview_test.jpg";
        GDALDatasetH jpg = GDALCreateCopy(GDALGetDriverByName("JPEG"), vsi.c_str(), mem, FALSE, nullptr, nullptr, nullptr);
        GDALClose(jpg);
        GDALClose(mem);

        vsi_l_offset size = 0;
        GByte *data = VSIGetMemFileBuffer(vsi.c_str(), &size, FALSE);
        std::vector<uint8_t> bytes(data, data + size);
        VSIUnlink(vsi.c_str());
        return bytes;
    }

    void putU16(std::vector<uint8_t> &v, uint16_t x) { v.insert(v.end(), {uint8_t(x & 0xFF), uint8_t(x >> 8)}); }
    void putU32(std::vector<uint8_t> &v, uint32_t x)
    {
        for (int i = 0; i < 4; i++)
            v.push_back(uint8_t((x >> (8 * i)) & 0xFF));
    }

    // A JPEG file with the layout DJI cameras write: the main image with an MPF index in
    // APP2 right after SOI, then the preview. previewOffsetShift moves the preview's
    // recorded offset (a stale index, as left by an editor that re-saved the file).
    std::vector<uint8_t> mpfJpeg(const std::vector<uint8_t> &main, const std::vector<uint8_t> &preview,
                                 uint32_t previewOffsetShift = 0)
    {
        constexpr uint32_t AppSegmentBytes = 78;   // FFE2 + length + "MPF\0" + 70 bytes of index
        constexpr uint32_t TiffStart = 2 + 4 + 4;  // SOI, marker + length, "MPF\0"
        const uint32_t mainSize = static_cast<uint32_t>(main.size()) + AppSegmentBytes;

        std::vector<uint8_t> tiff = {'I', 'I', 0x2A, 0x00};
        putU32(tiff, 8);  // first IFD
        putU16(tiff, 2);  // entries
        putU16(tiff, 0xB000); putU16(tiff, 7); putU32(tiff, 4); tiff.insert(tiff.end(), {'0', '1', '0', '0'});
        putU16(tiff, 0xB002); putU16(tiff, 7); putU32(tiff, 32); putU32(tiff, 38);
        putU32(tiff, 0);  // next IFD
        putU32(tiff, 0x20030000); putU32(tiff, mainSize); putU32(tiff, 0); putU32(tiff, 0);
        putU32(tiff, 0x00010001); putU32(tiff, static_cast<uint32_t>(preview.size()));
        putU32(tiff, mainSize - TiffStart + previewOffsetShift); putU32(tiff, 0);
        EXPECT_EQ(tiff.size(), 70u);

        std::vector<uint8_t> file = {0xFF, 0xD8, 0xFF, 0xE2, 0x00, 76, 'M', 'P', 'F', 0x00};
        file.insert(file.end(), tiff.begin(), tiff.end());
        file.insert(file.end(), main.begin() + 2, main.end());  // without its SOI
        EXPECT_EQ(file.size(), mainSize);
        file.insert(file.end(), preview.begin(), preview.end());
        return file;
    }

    fs::path writeFile(const fs::path &path, const std::vector<uint8_t> &bytes)
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return path;
    }

    DecodedImage thumbnail(const fs::path &image, int size)
    {
        uint8_t *buf = nullptr;
        int bufSize = 0;
        generateThumb(image, size, "", true, &buf, &bufSize);
        return decodeImage(takeBuffer(buf, bufSize));
    }

    // Colour at the centre of a decoded thumbnail
    Rgb centre(const DecodedImage &img)
    {
        const size_t i = static_cast<size_t>(img.height / 2) * img.width + img.width / 2;
        return {img.bands[0][i], img.bands[1][i], img.bands[2][i]};
    }

    bool isBlue(Rgb c) { return c.b > 150 && c.r < 100; }
    bool isRed(Rgb c) { return c.r > 150 && c.b < 100; }

    TEST(DDB10_Preview, layoutListsTheMpfPreview)
    {
        TestArea ta(TEST_NAME, true);
        const auto file = writeFile(ta.getPath("dji.jpg"), mpfJpeg(solidJpeg(1600, 1200, Red), solidJpeg(800, 600, Blue)));

        const auto layout = readJpegLayout(file);
        EXPECT_EQ(layout.width, 1600);
        EXPECT_EQ(layout.height, 1200);
        ASSERT_EQ(layout.previews.size(), 1u);
        EXPECT_EQ(layout.previews[0].width, 800);
        EXPECT_EQ(layout.previews[0].height, 600);
    }

    TEST(DDB10_Preview, thumbnailDecodesThePreview)
    {
        TestArea ta(TEST_NAME, true);
        const auto file = writeFile(ta.getPath("dji.jpg"), mpfJpeg(solidJpeg(1600, 1200, Red), solidJpeg(800, 600, Blue)));

        const auto img = thumbnail(file, 256);
        EXPECT_EQ(img.width, 256);
        EXPECT_EQ(img.height, 192);
        EXPECT_TRUE(isBlue(centre(img))) << "the preview was not used";
    }

    TEST(DDB10_Preview, previewSmallerThanTheThumbnailIsNotUsed)
    {
        TestArea ta(TEST_NAME, true);
        const auto file = writeFile(ta.getPath("dji.jpg"), mpfJpeg(solidJpeg(1600, 1200, Red), solidJpeg(400, 300, Blue)));

        const auto img = thumbnail(file, 512);
        EXPECT_EQ(img.width, 512);
        EXPECT_EQ(img.height, 384);
        EXPECT_TRUE(isRed(centre(img)));
    }

    TEST(DDB10_Preview, previewOfAnotherFramingIsNotUsed)
    {
        TestArea ta(TEST_NAME, true);
        const auto file = writeFile(ta.getPath("dji.jpg"), mpfJpeg(solidJpeg(1600, 1200, Red), solidJpeg(800, 450, Blue)));

        EXPECT_FALSE(pickJpegPreview(readJpegLayout(file), 256).has_value());
        EXPECT_TRUE(isRed(centre(thumbnail(file, 256))));
    }

    TEST(DDB10_Preview, staleIndexIsIgnored)
    {
        TestArea ta(TEST_NAME, true);
        const auto preview = solidJpeg(800, 600, Blue);

        // Points past the end of the file
        const auto past = writeFile(ta.getPath("past.jpg"), mpfJpeg(solidJpeg(1600, 1200, Red), preview, 4096));
        EXPECT_TRUE(readJpegLayout(past).previews.empty());
        EXPECT_TRUE(isRed(centre(thumbnail(past, 256))));

        // Points inside the file, but not at a JPEG image
        const auto inside = writeFile(ta.getPath("inside.jpg"), mpfJpeg(solidJpeg(1600, 1200, Red), preview, static_cast<uint32_t>(-100)));
        EXPECT_TRUE(readJpegLayout(inside).previews.empty());
        EXPECT_TRUE(isRed(centre(thumbnail(inside, 256))));
    }

    TEST(DDB10_Preview, plainJpegAndOtherFiles)
    {
        TestArea ta(TEST_NAME, true);
        const auto plain = writeFile(ta.getPath("plain.jpg"), solidJpeg(640, 480, Red));
        const auto layout = readJpegLayout(plain);
        EXPECT_EQ(layout.width, 640);
        EXPECT_EQ(layout.height, 480);
        EXPECT_TRUE(layout.previews.empty());

        const auto text = writeFile(ta.getPath("text.jpg"), {'h', 'e', 'l', 'l', 'o'});
        EXPECT_EQ(readJpegLayout(text).width, 0);
    }
}
