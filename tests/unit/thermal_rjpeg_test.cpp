/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Radiometric R-JPEGs (U-603: FR-RAS-13, FR-RAS-14, FR-RAS-15, FR-XC-24, D-012, D-022). A DJI R-JPEG carries the raw
// thermal image of the camera in its APP3 segments. These tests write such pictures (a JPEG, an EXIF block with the
// make and model, the raw image split in APP3 segments) so that no download is needed; the DJI thermal SDK samples
// (M2EA, H20T, H20N) are in tests/integration/thermal_test.cpp. The expected temperatures are literals computed
// outside the code under test: the Planck formula of the default calibration (R1 21106.77, B 1501, F 1, O -7340,
// R2 0.012545258) of the raw counts 15000 -> 4.9913 C, 15020 -> 5.1252 C, 15100 -> 5.6584 C, 15120 -> 5.7911 C,
// 16100 -> 12.0484 C, 22240 -> 43.8201 C.

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"

#include "ddb.h"
#include "entry.h"
#include "gdal_inc.h"
#include "json.h"
#include "raster_analysis.h"
#include "raster_test_utils.h"
#include "thermal.h"

namespace
{
    using namespace ddb;

    const char *const kCelsius = "\xC2\xB0" "C";

    // The raw count of the pixel (x, y) of the pictures written here: 15000 at the top left, 100 more per column,
    // 20 more per row
    uint16_t rawAt(int x, int y)
    {
        return static_cast<uint16_t>(15000 + 100 * x + 20 * y);
    }

    constexpr int kWidth = 64;
    constexpr int kHeight = 48;

    // The grey of the colour preview: a thermal picture is a picture that shows its temperatures in colours, and
    // what the raster API must not take for a temperature
    constexpr int kPreviewGrey = 147;

    std::vector<uint8_t> readAll(const fs::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    // The EXIF APP1 segment of a picture: Make and Model, in a little-endian TIFF with one IFD
    std::vector<uint8_t> exifSegment(const std::string &make, const std::string &model)
    {
        std::vector<uint8_t> tiff = {'I', 'I', 0x2A, 0x00, 0x08, 0x00, 0x00, 0x00};
        const auto put16 = [&](uint16_t v) {
            tiff.push_back(static_cast<uint8_t>(v & 0xFF));
            tiff.push_back(static_cast<uint8_t>(v >> 8));
        };
        const auto put32 = [&](uint32_t v) {
            for (int i = 0; i < 4; i++)
                tiff.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
        };
        const uint32_t dataStart = 8 + 2 + 2 * 12 + 4;

        put16(2); // two entries
        const std::string makeText = make + '\0';
        const std::string modelText = model + '\0';
        const auto entry = [&](uint16_t tag, const std::string &text, uint32_t offset) {
            put16(tag);
            put16(2); // ASCII
            put32(static_cast<uint32_t>(text.size()));
            if (text.size() <= 4)
            {
                std::string padded = text;
                padded.resize(4, '\0');
                tiff.insert(tiff.end(), padded.begin(), padded.end());
            }
            else
            {
                put32(offset);
            }
        };
        entry(0x010F, makeText, dataStart);
        entry(0x0110, modelText, dataStart);
        put32(0);
        tiff.insert(tiff.end(), modelText.begin(), modelText.end());

        std::vector<uint8_t> segment = {0xFF, 0xE1};
        const size_t length = 2 + 6 + tiff.size();
        segment.push_back(static_cast<uint8_t>(length >> 8));
        segment.push_back(static_cast<uint8_t>(length & 0xFF));
        const std::string exif("Exif\0\0", 6);
        segment.insert(segment.end(), exif.begin(), exif.end());
        segment.insert(segment.end(), tiff.begin(), tiff.end());
        return segment;
    }

    // A JPEG of kWidth x kHeight pixels, all kPreviewGrey, written by GDAL
    std::vector<uint8_t> plainJpeg(const fs::path &scratch)
    {
        GDALDriverH mem = GDALGetDriverByName("MEM");
        GDALDatasetH src = GDALCreate(mem, "", kWidth, kHeight, 3, GDT_Byte, nullptr);
        std::vector<uint8_t> grey(static_cast<size_t>(kWidth) * kHeight, kPreviewGrey);
        for (int b = 1; b <= 3; b++)
            EXPECT_EQ(GDALRasterIO(GDALGetRasterBand(src, b), GF_Write, 0, 0, kWidth, kHeight, grey.data(), kWidth,
                                   kHeight, GDT_Byte, 0, 0),
                      CE_None);
        char *options[] = {const_cast<char *>("QUALITY=100"), nullptr};
        GDALDatasetH jpeg = GDALCreateCopy(GDALGetDriverByName("JPEG"), scratch.string().c_str(), src, FALSE, options,
                                           nullptr, nullptr);
        EXPECT_NE(jpeg, nullptr);
        GDALClose(jpeg);
        GDALClose(src);
        auto bytes = readAll(scratch);
        fs::remove(scratch);
        fs::remove(fs::path(scratch.string() + ".aux.xml"));
        return bytes;
    }

    // A picture of a DJI camera: the EXIF block with its make and model, and, when raw is given, the raw thermal image
    // (little-endian UInt16) split in APP3 segments of at most chunk bytes. The JPEG data follows
    fs::path writeDjiPicture(const fs::path &path, const std::string &make, const std::string &model,
                             const std::vector<uint16_t> &raw, size_t chunk = 2048)
    {
        const auto jpeg = plainJpeg(path.string() + ".tmp.jpg");
        std::vector<uint8_t> out = {0xFF, 0xD8};
        const auto exif = exifSegment(make, model);
        out.insert(out.end(), exif.begin(), exif.end());

        std::vector<uint8_t> bytes;
        for (const uint16_t v : raw)
        {
            bytes.push_back(static_cast<uint8_t>(v & 0xFF));
            bytes.push_back(static_cast<uint8_t>(v >> 8));
        }
        for (size_t at = 0; at < bytes.size(); at += chunk)
        {
            const size_t n = std::min(chunk, bytes.size() - at);
            const size_t length = n + 2;
            out.push_back(0xFF);
            out.push_back(0xE3);
            out.push_back(static_cast<uint8_t>(length >> 8));
            out.push_back(static_cast<uint8_t>(length & 0xFF));
            out.insert(out.end(), bytes.begin() + at, bytes.begin() + at + n);
        }
        out.insert(out.end(), jpeg.begin() + 2, jpeg.end());

        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char *>(out.data()), static_cast<std::streamsize>(out.size()));
        return path;
    }

    std::vector<uint16_t> rawGrid(int width, int height)
    {
        std::vector<uint16_t> raw;
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++)
                raw.push_back(rawAt(x, y));
        return raw;
    }

    json pointOf(const fs::path &path, int x, int y, const char *formula = nullptr)
    {
        DDBRasterOptions options = {};
        options.structSize = sizeof(options);
        options.formula = formula;
        char *output = nullptr;
        const DDBErr err = DDBGetRasterPointValuePixelEx(path.string().c_str(), x, y, &options, &output);
        EXPECT_EQ(err, DDBERR_NONE) << DDBGetLastError();
        if (err != DDBERR_NONE)
            return json();
        const json result = json::parse(std::string(output));
        DDBFree(output);
        return result;
    }

    json valueInfoOf(const fs::path &path)
    {
        char *output = nullptr;
        const DDBErr err = DDBGetRasterValueInfo(path.string().c_str(), &output);
        EXPECT_EQ(err, DDBERR_NONE) << DDBGetLastError();
        if (err != DDBERR_NONE)
            return json();
        const json result = json::parse(std::string(output));
        DDBFree(output);
        return result;
    }

    // ---------------------------------------------------------------- FR-RAS-13: the DJI raw thermal probe

    TEST(RJpegThermal, theProbeFindsTheRawThermalImageOfAnyDjiModel)
    {
        TestArea ta(TEST_NAME);
        // A model that is in no list: the raw thermal image in the file says that it is a thermal picture
        const auto path = writeDjiPicture(ta.getPath("DJI_0001_R.JPG"), "DJI", "ZZ-NEW-THERMAL-9", rawGrid(kWidth, kHeight));

        EXPECT_FALSE(isThermalImageFromExif("DJI", "ZZ-NEW-THERMAL-9"));
        EXPECT_TRUE(hasDjiRawThermal(path.string()));
        EXPECT_TRUE(isThermalImage(path.string()));
        EXPECT_TRUE(extractThermalCalibration(path.string()).valid);

        const RawThermalData raw = extractRawThermalData(path.string());
        ASSERT_TRUE(raw.valid);
        EXPECT_EQ(raw.width, kWidth);
        EXPECT_EQ(raw.height, kHeight);
        ASSERT_EQ(raw.data.size(), static_cast<size_t>(kWidth) * kHeight);
        EXPECT_EQ(raw.data[0], 15000);
        EXPECT_EQ(raw.data[10 + 5 * kWidth], 16100);
        EXPECT_EQ(raw.data[63 + 47 * kWidth], 22240);
    }

    TEST(RJpegThermal, aDjiPhotoWithoutRawThermalDataIsNotThermal)
    {
        TestArea ta(TEST_NAME);
        const auto path = writeDjiPicture(ta.getPath("DJI_0002_W.JPG"), "DJI", "FC220", {});

        EXPECT_FALSE(hasDjiRawThermal(path.string()));
        EXPECT_FALSE(isThermalImage(path.string()));
        EXPECT_FALSE(extractRawThermalData(path.string()).valid);
    }

    TEST(RJpegThermal, theRawImageOfTheNominalSensorIsReadWhateverThePictureSize)
    {
        TestArea ta(TEST_NAME);
        // The picture is 64 x 48, the segments hold 640 x 512 counts (the M4T has a larger picture than its thermal
        // image): 640 * 512 * 2 bytes, in the 65533-byte payloads of the APP3 segments
        std::vector<uint16_t> raw(static_cast<size_t>(640) * 512, 17000);
        raw[5 + 3 * 640] = 19000;
        const auto path = writeDjiPicture(ta.getPath("DJI_0003_R.JPG"), "DJI", "ZZ-NEW-THERMAL-9", raw, 65533);

        const RawThermalData read = extractRawThermalData(path.string());
        ASSERT_TRUE(read.valid);
        EXPECT_EQ(read.width, 640);
        EXPECT_EQ(read.height, 512);
        EXPECT_EQ(read.data[5 + 3 * 640], 19000);
        EXPECT_EQ(read.data[0], 17000);
    }

    TEST(RJpegThermal, segmentsOfAnotherSizeAreNoRawThermalImage)
    {
        TestArea ta(TEST_NAME);
        // 1000 counts for a 64 x 48 picture: the GDAL rule does not accept them, nor does the probe
        const auto path = writeDjiPicture(ta.getPath("DJI_0004_R.JPG"), "DJI", "ZZ-NEW-THERMAL-9",
                                          std::vector<uint16_t>(1000, 17000));

        EXPECT_FALSE(hasDjiRawThermal(path.string()));
        EXPECT_FALSE(extractRawThermalData(path.string()).valid);
    }

    TEST(RJpegThermal, theCamerasOfThe2022DjiLineupAreListed)
    {
        // The models of the DJI thermal SDK samples and of the cameras that came after them
        EXPECT_TRUE(isThermalImageFromExif("DJI", "MAVIC2-ENTERPRISE-ADVANCED"));
        EXPECT_TRUE(isThermalImageFromExif("DJI", "ZH20N"));
        EXPECT_TRUE(isThermalImageFromExif("DJI", "ZH20T"));
        EXPECT_TRUE(isThermalImageFromExif("DJI", "M30T"));
        EXPECT_TRUE(isThermalImageFromExif("DJI", "M3T"));
        EXPECT_TRUE(isThermalImageFromExif("DJI", "M3TD"));
        EXPECT_TRUE(isThermalImageFromExif("DJI", "H30T"));
        // The Mavic 3 Enterprise has no thermal camera (the M3T does): it is a plain image
        EXPECT_FALSE(isThermalImageFromExif("DJI", "M3E"));
        EXPECT_FALSE(isThermalImageFromExif("DJI", "Mavic 3 Enterprise"));
        EXPECT_FALSE(isThermalImageFromExif("DJI", "FC220"));
    }

    TEST(RJpegThermal, theEntryOfAThermalDjiPictureOfAnyModelIsMarkedThermal)
    {
        TestArea ta(TEST_NAME);
        const auto thermal = writeDjiPicture(ta.getPath("DJI_0001_R.JPG"), "DJI", "ZZ-NEW-THERMAL-9", rawGrid(kWidth, kHeight));
        const auto visual = writeDjiPicture(ta.getPath("DJI_0002_W.JPG"), "DJI", "FC220", {});

        Entry thermalEntry;
        parseEntry(thermal, ta.getFolder(), thermalEntry, false);
        EXPECT_EQ(thermalEntry.properties.value("sensorCategory", ""), "thermal");

        Entry visualEntry;
        parseEntry(visual, ta.getFolder(), visualEntry, false);
        EXPECT_EQ(visualEntry.properties.value("sensorCategory", ""), "");
    }

    // ---------------------------------------------------------------- FR-RAS-14: the temperatures of the raw image

    TEST(RJpegThermal, theValueInfoOfAnRJpegHasTheTemperaturesOfItsRawImage)
    {
        TestArea ta(TEST_NAME);
        const auto path = writeDjiPicture(ta.getPath("DJI_0001_R.JPG"), "DJI", "ZZ-NEW-THERMAL-9", rawGrid(kWidth, kHeight));

        const json info = valueInfoOf(path);
        EXPECT_EQ(info["width"], kWidth);
        EXPECT_EQ(info["height"], kHeight);
        EXPECT_TRUE(info["isThermal"].get<bool>());
        EXPECT_FALSE(info["isDirectValue"].get<bool>());
        EXPECT_EQ(info["unit"], kCelsius);
        // The range is that of the temperatures of the raw counts 15000 and 22240, not 0-255 of the colour preview
        EXPECT_NEAR(info["valueMin"].get<double>(), 4.9913, 0.01);
        EXPECT_NEAR(info["valueMax"].get<double>(), 43.8201, 0.01);
        ASSERT_TRUE(info.contains("calibration"));
        EXPECT_GT(info["calibration"]["emissivity"].get<double>(), 0.0);
        EXPECT_GT(info["calibration"]["objectDistance"].get<double>(), 0.0);
        ASSERT_EQ(info["stretch"].size(), 1u);
        EXPECT_NEAR(info["stretch"][0]["min"].get<double>(), 4.9913, 0.01);
        EXPECT_NEAR(info["stretch"][0]["max"].get<double>(), 43.8201, 0.01);

        // A thermal formula gives the range in its unit (D-022)
        RasterValueOptions kelvin;
        kelvin.formula = "KELVIN";
        const json inKelvin = json::parse(getRasterValueInfoJson(path.string(), kelvin));
        EXPECT_EQ(inKelvin["unit"], "K");
        EXPECT_NEAR(inKelvin["valueMin"].get<double>(), 278.1413, 0.01);
        EXPECT_NEAR(inKelvin["valueMax"].get<double>(), 316.9701, 0.01);
    }

    TEST(RJpegThermal, thePointOfAnRJpegIsATemperatureNotTheColourOfItsPreview)
    {
        TestArea ta(TEST_NAME);
        const auto path = writeDjiPicture(ta.getPath("DJI_0001_R.JPG"), "DJI", "ZZ-NEW-THERMAL-9", rawGrid(kWidth, kHeight));

        const json point = pointOf(path, 10, 5);
        EXPECT_NEAR(point["value"].get<double>(), 12.0484, 0.01);
        EXPECT_NE(static_cast<int>(point["value"].get<double>()), kPreviewGrey);
        EXPECT_EQ(point["rawValue"], 16100);
        EXPECT_EQ(point["unit"], kCelsius);
        EXPECT_EQ(point["x"], 10);
        EXPECT_EQ(point["y"], 5);
        EXPECT_TRUE(point["isThermal"].get<bool>());
        EXPECT_FALSE(point["hasGeo"].get<bool>());

        EXPECT_NEAR(pointOf(path, 63, 47)["value"].get<double>(), 43.8201, 0.01);
        const json kelvin = pointOf(path, 0, 0, "KELVIN");
        EXPECT_EQ(kelvin["unit"], "K");
        EXPECT_NEAR(kelvin["value"].get<double>(), 278.1413, 0.01);

        // A pixel outside the raw grid is outside the raster
        char *output = nullptr;
        EXPECT_NE(DDBGetRasterPointValuePixelEx(path.string().c_str(), kWidth, 0, nullptr, &output), DDBERR_NONE);
    }

    TEST(RJpegThermal, theAreaOfAnRJpegHasTheStatisticsOfItsTemperatures)
    {
        TestArea ta(TEST_NAME);
        const auto path = writeDjiPicture(ta.getPath("DJI_0001_R.JPG"), "DJI", "ZZ-NEW-THERMAL-9", rawGrid(kWidth, kHeight));

        char *output = nullptr;
        ASSERT_EQ(DDBGetRasterAreaStatsPixelEx(path.string().c_str(), 0, 0, 1, 1, nullptr, &output), DDBERR_NONE)
            << DDBGetLastError();
        const json area = json::parse(std::string(output));
        DDBFree(output);
        EXPECT_EQ(area["pixelCount"], 4);
        EXPECT_EQ(area["unit"], kCelsius);
        EXPECT_NEAR(area["min"].get<double>(), 4.9913, 0.01);
        EXPECT_NEAR(area["max"].get<double>(), 5.7911, 0.01);
        EXPECT_NEAR(area["mean"].get<double>(), 5.3915, 0.01);
    }

    TEST(RJpegThermal, theColourPreviewOfAThermalPictureWithoutRawDataIsNotDegrees)
    {
        TestArea ta(TEST_NAME);
        // The model of a thermal camera, but no raw thermal image: the bands GDAL reads are the 8-bit colours of the
        // preview. They are no temperatures: no unit, no calibration, not direct values (FR-RAS-14)
        const auto path = writeDjiPicture(ta.getPath("DJI_0005_T.JPG"), "DJI", "ZH20T", {});
        ASSERT_TRUE(isThermalImage(path.string()));

        const json info = valueInfoOf(path);
        EXPECT_TRUE(info["isThermal"].get<bool>());
        EXPECT_FALSE(info["isDirectValue"].get<bool>());
        EXPECT_EQ(info["unit"], "");

        const json point = pointOf(path, 10, 5);
        EXPECT_EQ(point["unit"], "");
        EXPECT_EQ(point["value"], kPreviewGrey);
    }

    // ---------------------------------------------------------------- FR-RAS-15: the thumbnail colours the temperatures

    // The thumbnail of the picture at its own size, 64 pixels, with a thermal formula or none and a colormap
    testutils::DecodedImage thumbnailOf(const fs::path &path, const char *formula, const char *colormap,
                                        const char *rescale)
    {
        uint8_t *buffer = nullptr;
        int size = 0;
        const DDBErr err = DDBGenerateMemoryThumbnailEx(path.string().c_str(), kWidth, nullptr, nullptr, formula,
                                                        nullptr, colormap, rescale, &buffer, &size);
        EXPECT_EQ(err, DDBERR_NONE) << DDBGetLastError();
        if (err != DDBERR_NONE)
            return {};
        std::vector<uint8_t> bytes(buffer, buffer + size);
        DDBVSIFree(buffer);
        return testutils::decodeImage(bytes);
    }

    // The colour of the pixel (x, y) of a decoded image: red, green, blue
    std::vector<int> colourAt(const testutils::DecodedImage &image, int x, int y)
    {
        const size_t i = static_cast<size_t>(y) * image.width + x;
        return {image.bands[0][i], image.bands[1][i], image.bands[2][i]};
    }

    // The thumbnail is a JPEG: its colours are those of the colormap within the loss of the compression
    void expectColour(const testutils::DecodedImage &image, int x, int y, const std::vector<int> &expected)
    {
        const std::vector<int> actual = colourAt(image, x, y);
        for (size_t c = 0; c < 3; c++)
            EXPECT_NEAR(actual[c], expected[c], 14) << "channel " << c << " of the pixel (" << x << ", " << y << ")";
    }

    TEST(RJpegThermal, theThumbnailWithAColormapColoursTheRawTemperatures)
    {
        TestArea ta(TEST_NAME);
        const auto path = writeDjiPicture(ta.getPath("DJI_0001_R.JPG"), "DJI", "ZZ-NEW-THERMAL-9", rawGrid(kWidth, kHeight));

        // "rainbow" goes from (150, 0, 90) to (200, 0, 0). The hottest pixel has its last colour and the coldest its
        // first, whatever the grey of the preview is
        const auto image = thumbnailOf(path, "CELSIUS", "rainbow", nullptr);
        ASSERT_EQ(image.width, kWidth);
        ASSERT_EQ(image.height, kHeight);
        expectColour(image, 63, 47, {200, 0, 0});
        expectColour(image, 0, 0, {150, 0, 90});
        EXPECT_NE(colourAt(image, 32, 24), (std::vector<int>{kPreviewGrey, kPreviewGrey, kPreviewGrey}));

        // No formula: the temperatures in degrees Celsius, the same picture
        const auto sameImage = thumbnailOf(path, nullptr, "rainbow", nullptr);
        expectColour(sameImage, 63, 47, {200, 0, 0});
        expectColour(sameImage, 0, 0, {150, 0, 90});
    }

    TEST(RJpegThermal, theRescaleOfTheThumbnailIsInTheUnitOfTheFormula)
    {
        TestArea ta(TEST_NAME);
        const auto path = writeDjiPicture(ta.getPath("DJI_0001_R.JPG"), "DJI", "ZZ-NEW-THERMAL-9", rawGrid(kWidth, kHeight));

        // 4.9913 C is the coldest pixel, 12.0484 C the pixel (10, 5): a range of 20 to 30 C puts both under the
        // first colour, and the 26.667 C pixel (32, 24) at (26.667 - 20) / 10 of the map
        const auto celsius = thumbnailOf(path, "CELSIUS", "grayscale", "20,30");
        expectColour(celsius, 0, 0, {0, 0, 0});
        expectColour(celsius, 10, 5, {0, 0, 0});
        expectColour(celsius, 63, 47, {255, 255, 255});
        EXPECT_NEAR(colourAt(celsius, 32, 24)[0], 0.6667 * 255, 8.0);

        // The same range in K (293.15 to 303.15 K) is the same picture
        const auto kelvin = thumbnailOf(path, "KELVIN", "grayscale", "293.15,303.15");
        EXPECT_NEAR(colourAt(kelvin, 32, 24)[0], colourAt(celsius, 32, 24)[0], 4.0);
        expectColour(kelvin, 0, 0, {0, 0, 0});
        expectColour(kelvin, 63, 47, {255, 255, 255});
    }
} // namespace
