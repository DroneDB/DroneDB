/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Radiometric R-JPEGs (U-603: FR-RAS-13, FR-RAS-14, FR-RAS-15, FR-XC-24, D-012, D-022). A DJI R-JPEG carries the raw
// thermal image of the camera in its APP3 segments; a FLIR R-JPEG carries it, and its Planck calibration, in the
// RawData and CameraInfo records of the FFF data of its FLIR APP1 segments (older writers: a raw block in the APP1
// segment and the calibration in the XMP). These tests write such pictures (a JPEG, an EXIF block with the make and
// model, the raw image) so that no download is needed; the DJI thermal SDK samples (M2EA, H20T, H20N) are in
// tests/integration/thermal_test.cpp. The raw counts of a DJI camera need DJI's own conversion, which DroneDB does not
// have: the raster API gives them as raw counts without a unit. The expected temperatures of the FLIR picture are
// literals computed outside the code under test: the Planck formula (R1 21106.77, B 1501, F 1, O -7340, R2 0.012545258)
// of the raw counts 15104 -> 5.6850 C, 15124 -> 5.8177 C, 15204 -> 6.3464 C, 15224 -> 6.4780 C, 16204 -> 12.6860 C,
// 18784 -> 27.2110 C, 22344 -> 44.2823 C.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
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
#include "thermal_internal.h"
#include "user_open_child.h"

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

    // An APP1 segment: the marker, the length and the payload
    std::vector<uint8_t> app1(const std::string &payload)
    {
        std::vector<uint8_t> segment = {0xFF, 0xE1};
        const size_t length = payload.size() + 2;
        segment.push_back(static_cast<uint8_t>(length >> 8));
        segment.push_back(static_cast<uint8_t>(length & 0xFF));
        segment.insert(segment.end(), payload.begin(), payload.end());
        return segment;
    }

    // The raw count of the pixel (x, y) of the FLIR picture: 15104 at the top left (its low byte is 0, so the block is
    // found where it starts), 100 more per column, 20 more per row
    uint16_t flirRawAt(int x, int y)
    {
        return static_cast<uint16_t>(15104 + 100 * x + 20 * y);
    }

    // A picture of a FLIR camera: the EXIF block, an XMP packet with the FLIR tags given (name -> value) and, when
    // withRaw, the FLIR APP1 segment with the raw thermal image (flirRawAt, little-endian UInt16)
    fs::path writeFlirPicture(const fs::path &path, const std::vector<std::pair<std::string, std::string>> &flirTags,
                              bool withRaw = true)
    {
        const auto jpeg = plainJpeg(path.string() + ".tmp.jpg");
        std::vector<uint8_t> out = {0xFF, 0xD8};
        const auto exif = exifSegment("FLIR Systems AB", "FLIR Vue Pro R");
        out.insert(out.end(), exif.begin(), exif.end());

        std::string xmp = std::string("http://ns.adobe.com/xap/1.0/") + '\0' +
                          "<?xpacket begin='' id='W5M0MpCehiHzreSzNTczkc9d'?>"
                          "<x:xmpmeta xmlns:x='adobe:ns:meta/'><rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>"
                          "<rdf:Description rdf:about='' xmlns:FLIR='http://ns.flir.com/xmp/1.0/'";
        for (const auto &[name, value] : flirTags)
            xmp += " FLIR:" + name + "='" + value + "'";
        xmp += "/></rdf:RDF></x:xmpmeta><?xpacket end='w'?>";
        const auto xmpSegment = app1(xmp);
        out.insert(out.end(), xmpSegment.begin(), xmpSegment.end());

        if (withRaw)
        {
            // "FLIR\0", 3 bytes of the segment index (zero), 16 bytes of zero, then the raw image
            std::string flir("FLIR\0\0\0\0", 8);
            flir.append(16, '\0');
            for (int y = 0; y < kHeight; y++)
                for (int x = 0; x < kWidth; x++)
                {
                    const uint16_t v = flirRawAt(x, y);
                    flir.push_back(static_cast<char>(v & 0xFF));
                    flir.push_back(static_cast<char>(v >> 8));
                }
            const auto flirSegment = app1(flir);
            out.insert(out.end(), flirSegment.begin(), flirSegment.end());
        }
        out.insert(out.end(), jpeg.begin() + 2, jpeg.end());

        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char *>(out.data()), static_cast<std::streamsize>(out.size()));
        return path;
    }

    // The Planck constants of the FLIR pictures, and the measurement parameters the camera says (no humidity, no
    // reflected temperature: they must not be reported)
    const std::vector<std::pair<std::string, std::string>> kFlirCalibration = {
        {"PlanckR1", "21106.77"}, {"PlanckB", "1501"}, {"PlanckF", "1"}, {"PlanckO", "-7340"},
        {"PlanckR2", "0.012545258"}, {"Emissivity", "0.95"}, {"ObjectDistance", "5"},
        {"RawThermalImageWidth", "64"}, {"RawThermalImageHeight", "48"}};

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

    // The error of a point value with a formula: the code, and the message
    std::pair<DDBErr, std::string> pointErrorOf(const fs::path &path, int x, int y, const char *formula)
    {
        DDBRasterOptions options = {};
        options.structSize = sizeof(options);
        options.formula = formula;
        char *output = nullptr;
        const DDBErr err = DDBGetRasterPointValuePixelEx(path.string().c_str(), x, y, &options, &output);
        if (err == DDBERR_NONE)
            DDBFree(output);
        return {err, err == DDBERR_NONE ? std::string() : std::string(DDBGetLastError())};
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
        // No calibration: the raw counts of a DJI camera need DJI's own conversion, not the Planck formula of FLIR
        EXPECT_FALSE(extractThermalCalibration(path.string()).valid);

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

    // The wide and zoom pictures of a DJI thermal drone have the model of its thermal camera: without the raw thermal
    // image, a DJI picture is not thermal, whatever its model (FR-RAS-13)
    TEST(RJpegThermal, aDjiPictureWithTheModelOfAThermalCameraButNoRawImageIsNotThermal)
    {
        TestArea ta(TEST_NAME);
        for (const char *model : {"M30T", "M3T", "MAVIC2-ENTERPRISE-ADVANCED", "ZH20N", "ZH20T", "ZH30T"})
        {
            SCOPED_TRACE(model);
            const auto path = writeDjiPicture(ta.getPath(std::string(model) + "_W.JPG"), "DJI", model, {});
            ASSERT_TRUE(isThermalImageFromExif("DJI", model));
            EXPECT_FALSE(isThermalPicture("DJI", model, path.string()));
            EXPECT_FALSE(hasThermalTags(path.string()));
            EXPECT_FALSE(isThermalImage(path.string()));

            Entry entry;
            parseEntry(path, ta.getFolder(), entry, false);
            EXPECT_EQ(entry.properties.value("sensorCategory", ""), "");
        }

        // The same model with its raw thermal image is thermal
        const auto thermal = writeDjiPicture(ta.getPath("M30T_T.JPG"), "DJI", "M30T", rawGrid(kWidth, kHeight));
        EXPECT_TRUE(isThermalPicture("DJI", "M30T", thermal.string()));
        Entry entry;
        parseEntry(thermal, ta.getFolder(), entry, false);
        EXPECT_EQ(entry.properties.value("sensorCategory", ""), "thermal");
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

    // ---------------------------------------------------------------- FR-RAS-14: the raw counts of a DJI R-JPEG

    TEST(RJpegThermal, theValueInfoOfADjiRJpegHasItsRawCountsWithoutAUnit)
    {
        TestArea ta(TEST_NAME);
        const auto path = writeDjiPicture(ta.getPath("DJI_0001_R.JPG"), "DJI", "ZZ-NEW-THERMAL-9", rawGrid(kWidth, kHeight));

        const json info = valueInfoOf(path);
        EXPECT_EQ(info["width"], kWidth);
        EXPECT_EQ(info["height"], kHeight);
        EXPECT_TRUE(info["isThermal"].get<bool>());
        EXPECT_FALSE(info["isDirectValue"].get<bool>());
        // The raw counts 15000 to 22240, without a unit: not temperatures, not 0-255 of the colour preview
        EXPECT_EQ(info["unit"], "");
        EXPECT_EQ(info["valueMin"], 15000);
        EXPECT_EQ(info["valueMax"], 22240);
        EXPECT_EQ(info["dataType"], "UInt16");
        EXPECT_FALSE(info.contains("calibration"));
        ASSERT_EQ(info["stretch"].size(), 1u);
        EXPECT_EQ(info["stretch"][0]["min"], 15000);
        EXPECT_EQ(info["stretch"][0]["max"], 22240);
        EXPECT_EQ(info["validPixelCount"], kWidth * kHeight);

        // A thermal formula gives raw counts no unit: the info says what the image has
        RasterValueOptions kelvin;
        kelvin.formula = "KELVIN";
        const json inKelvin = json::parse(getRasterValueInfoJson(path.string(), kelvin));
        EXPECT_EQ(inKelvin["unit"], "");
        EXPECT_EQ(inKelvin["valueMin"], 15000);
        EXPECT_EQ(inKelvin["valueMax"], 22240);
        EXPECT_FALSE(inKelvin.contains("calibration"));

        // The raw count 15000 as NoData: the range starts at the next one
        RasterValueOptions nodata;
        nodata.nodata = 15000;
        const json withNodata = json::parse(getRasterValueInfoJson(path.string(), nodata));
        EXPECT_EQ(withNodata["valueMin"], 15020);
        EXPECT_EQ(withNodata["validPixelCount"], kWidth * kHeight - 1);
    }

    TEST(RJpegThermal, thePointOfADjiRJpegIsItsRawCountNotTheColourOfItsPreview)
    {
        TestArea ta(TEST_NAME);
        const auto path = writeDjiPicture(ta.getPath("DJI_0001_R.JPG"), "DJI", "ZZ-NEW-THERMAL-9", rawGrid(kWidth, kHeight));

        const json point = pointOf(path, 10, 5);
        EXPECT_EQ(point["value"], 16100);
        EXPECT_EQ(point["rawValue"], 16100);
        EXPECT_EQ(point["unit"], "");
        EXPECT_EQ(point["x"], 10);
        EXPECT_EQ(point["y"], 5);
        EXPECT_TRUE(point["isThermal"].get<bool>());
        EXPECT_FALSE(point["hasGeo"].get<bool>());
        EXPECT_EQ(pointOf(path, 63, 47)["value"], 22240);

        // No temperature can be asked for: there is no calibration (DJI's conversion is not available), which the
        // refusal says
        const auto [err, message] = pointErrorOf(path, 0, 0, "KELVIN");
        EXPECT_EQ(err, DDBERR_INVALIDARGS);
        EXPECT_NE(message.find("No thermal calibration"), std::string::npos) << message;
        EXPECT_NE(message.find("raw sensor counts"), std::string::npos) << message;
        EXPECT_NE(message.find("DJI's own conversion"), std::string::npos) << message;
        // A formula that does not exist is refused as on any raster
        const auto [unknownErr, unknownMessage] = pointErrorOf(path, 0, 0, "NOPE");
        EXPECT_EQ(unknownErr, DDBERR_INVALIDARGS);
        EXPECT_NE(unknownMessage.find("Unknown formula"), std::string::npos) << unknownMessage;

        // A pixel outside the raw grid is outside the raster
        char *output = nullptr;
        EXPECT_NE(DDBGetRasterPointValuePixelEx(path.string().c_str(), kWidth, 0, nullptr, &output), DDBERR_NONE);
    }

    TEST(RJpegThermal, theAreaOfADjiRJpegHasTheStatisticsOfItsRawCounts)
    {
        TestArea ta(TEST_NAME);
        const auto path = writeDjiPicture(ta.getPath("DJI_0001_R.JPG"), "DJI", "ZZ-NEW-THERMAL-9", rawGrid(kWidth, kHeight));

        char *output = nullptr;
        ASSERT_EQ(DDBGetRasterAreaStatsPixelEx(path.string().c_str(), 0, 0, 1, 1, nullptr, &output), DDBERR_NONE)
            << DDBGetLastError();
        const json area = json::parse(std::string(output));
        DDBFree(output);
        // 15000, 15100, 15020, 15120
        EXPECT_EQ(area["pixelCount"], 4);
        EXPECT_EQ(area["unit"], "");
        EXPECT_EQ(area["min"], 15000);
        EXPECT_EQ(area["max"], 15120);
        EXPECT_NEAR(area["mean"].get<double>(), 15060, 0.01);
        EXPECT_TRUE(area["isThermal"].get<bool>());

        DDBRasterOptions celsius = {};
        celsius.structSize = sizeof(celsius);
        celsius.formula = "CELSIUS";
        output = nullptr;
        EXPECT_EQ(DDBGetRasterAreaStatsPixelEx(path.string().c_str(), 0, 0, 1, 1, &celsius, &output), DDBERR_INVALIDARGS);
    }

    TEST(RJpegThermal, theColourPreviewOfAThermalPictureWithoutRawDataIsNotDegrees)
    {
        TestArea ta(TEST_NAME);
        // A FLIR camera, but no raw thermal image and no calibration: the bands GDAL reads are the 8-bit colours of the
        // preview. They are no temperatures: no unit, no calibration, not direct values (FR-RAS-14)
        const auto path = writeFlirPicture(ta.getPath("FLIR_0005.JPG"), {{"Emissivity", "0.95"}}, false);
        ASSERT_TRUE(isThermalImage(path.string()));
        EXPECT_FALSE(extractThermalCalibration(path.string()).valid);

        const json info = valueInfoOf(path);
        EXPECT_TRUE(info["isThermal"].get<bool>());
        EXPECT_FALSE(info["isDirectValue"].get<bool>());
        EXPECT_EQ(info["unit"], "");
        EXPECT_FALSE(info.contains("calibration"));

        const json point = pointOf(path, 10, 5);
        EXPECT_EQ(point["unit"], "");
        EXPECT_EQ(point["value"], kPreviewGrey);

        // A thermal formula cannot make degrees of the colours (the byte 147 was 147 degrees Celsius): the refusal
        // says that they are the colours of the preview, not raw counts nor anything about DJI
        const auto [err, message] = pointErrorOf(path, 10, 5, "CELSIUS");
        EXPECT_EQ(err, DDBERR_INVALIDARGS);
        EXPECT_NE(message.find("No thermal calibration"), std::string::npos) << message;
        EXPECT_NE(message.find("colours of its preview"), std::string::npos) << message;
        EXPECT_EQ(message.find("raw sensor counts"), std::string::npos) << message;
        EXPECT_EQ(message.find("DJI"), std::string::npos) << message;
    }

    // ---------------------------------------------------------------- FLIR: the Planck calibration of its tags

    TEST(RJpegThermal, theValueInfoOfAFlirRJpegHasTheTemperaturesOfItsCalibration)
    {
        TestArea ta(TEST_NAME);
        const auto path = writeFlirPicture(ta.getPath("FLIR_0001.JPG"), kFlirCalibration);

        const ThermalCalibration cal = extractThermalCalibration(path.string());
        ASSERT_TRUE(cal.valid);
        EXPECT_DOUBLE_EQ(cal.planckR1, 21106.77);
        EXPECT_DOUBLE_EQ(cal.planckO, -7340.0);
        ASSERT_TRUE(cal.emissivity.has_value());
        EXPECT_DOUBLE_EQ(*cal.emissivity, 0.95);
        EXPECT_FALSE(cal.relativeHumidity.has_value());

        const json info = valueInfoOf(path);
        EXPECT_EQ(info["width"], kWidth);
        EXPECT_EQ(info["height"], kHeight);
        EXPECT_EQ(info["unit"], kCelsius);
        EXPECT_FALSE(info["isDirectValue"].get<bool>());
        EXPECT_NEAR(info["valueMin"].get<double>(), 5.6850, 0.01);
        EXPECT_NEAR(info["valueMax"].get<double>(), 44.2823, 0.01);

        // The calibration has the fields the file says, and only them (no default reported as the camera's)
        ASSERT_TRUE(info.contains("calibration"));
        const json &calibration = info["calibration"];
        EXPECT_DOUBLE_EQ(calibration["emissivity"].get<double>(), 0.95);
        EXPECT_DOUBLE_EQ(calibration["objectDistance"].get<double>(), 5.0);
        EXPECT_DOUBLE_EQ(calibration["planckB"].get<double>(), 1501.0);
        EXPECT_FALSE(calibration.contains("relativeHumidity"));
        EXPECT_FALSE(calibration.contains("reflectedApparentTemperature"));
        EXPECT_FALSE(calibration.contains("atmosphericTemperature"));

        const json point = pointOf(path, 10, 5);
        EXPECT_NEAR(point["value"].get<double>(), 12.6860, 0.01);
        EXPECT_EQ(point["rawValue"], 16204);
        EXPECT_EQ(point["unit"], kCelsius);
        const json kelvin = pointOf(path, 0, 0, "KELVIN");
        EXPECT_EQ(kelvin["unit"], "K");
        EXPECT_NEAR(kelvin["value"].get<double>(), 278.835, 0.01);

        char *output = nullptr;
        ASSERT_EQ(DDBGetRasterAreaStatsPixelEx(path.string().c_str(), 0, 0, 1, 1, nullptr, &output), DDBERR_NONE)
            << DDBGetLastError();
        const json area = json::parse(std::string(output));
        DDBFree(output);
        EXPECT_EQ(area["pixelCount"], 4);
        EXPECT_NEAR(area["min"].get<double>(), 5.6850, 0.01);
        EXPECT_NEAR(area["max"].get<double>(), 6.4780, 0.01);
        EXPECT_NEAR(area["mean"].get<double>(), 6.0817, 0.01);
    }

    // The relative humidity of the FLIR XMP is a fraction, as the one of the CameraInfo record: a percentage (over 2)
    // is divided by 100, so that one camera says one humidity whichever of the two the calibration comes from
    TEST(RJpegThermal, theRelativeHumidityOfTheFlirXmpIsAFractionAsTheOneOfTheCameraInfoRecord)
    {
        TestArea ta(TEST_NAME);
        int n = 0;
        for (const std::string humidity : {"50", "0.5"})
        {
            SCOPED_TRACE(humidity);
            auto tags = kFlirCalibration;
            tags.emplace_back("RelativeHumidity", humidity);
            const auto path = writeFlirPicture(ta.getPath("FLIR_RH_" + std::to_string(n++) + ".JPG"), tags);

            const ThermalCalibration cal = extractThermalCalibration(path.string());
            ASSERT_TRUE(cal.valid);
            ASSERT_TRUE(cal.relativeHumidity.has_value());
            EXPECT_NEAR(*cal.relativeHumidity, 0.5, 1e-9);

            const json info = valueInfoOf(path);
            ASSERT_TRUE(info.contains("calibration"));
            EXPECT_NEAR(info["calibration"]["relativeHumidity"].get<double>(), 0.5, 1e-9);
        }
    }

    // No real FLIR file has the temperatures of the measurement in its XMP (Xmp.FLIR.ReflectedApparentTemperature,
    // AtmosphericTemperature, IRWindowTemperature: the FLIR XMP of the Vue Pro R, Duo Pro R and Zenmuse XT, XT R and
    // XT2 has none of them), so their unit is not known (the CameraInfo record has K): they are not read. The emissivity
    // is applied with the reflected temperature only, the atmosphere with its temperature and the window with its
    // own, so an XMP calibration with all of them keeps the temperatures of its Planck constants (the literals at the
    // top of this file)
    TEST(RJpegThermal, theXmpTemperaturesOfAFlirPictureAreNotReadSoItsEmissivityLeavesThePlanckTemperatures)
    {
        TestArea ta(TEST_NAME);
        auto tags = kFlirCalibration; // Emissivity 0.95, ObjectDistance 5
        tags.emplace_back("ReflectedApparentTemperature", "20");
        tags.emplace_back("AtmosphericTemperature", "22");
        tags.emplace_back("RelativeHumidity", "0.5");
        tags.emplace_back("IRWindowTemperature", "20");
        tags.emplace_back("IRWindowTransmission", "0.8");
        const auto path = writeFlirPicture(ta.getPath("FLIR_XMP_TEMPS.JPG"), tags);

        const ThermalCalibration cal = extractThermalCalibration(path.string());
        ASSERT_TRUE(cal.valid);
        ASSERT_TRUE(cal.emissivity && cal.objectDistance && cal.relativeHumidity && cal.irWindowTransmission);
        EXPECT_DOUBLE_EQ(*cal.emissivity, 0.95);
        EXPECT_DOUBLE_EQ(*cal.objectDistance, 5.0);
        EXPECT_DOUBLE_EQ(*cal.relativeHumidity, 0.5);
        EXPECT_DOUBLE_EQ(*cal.irWindowTransmission, 0.8);
        EXPECT_FALSE(cal.reflectedApparentTemperature.has_value());
        EXPECT_FALSE(cal.atmosphericTemperature.has_value());
        EXPECT_FALSE(cal.irWindowTemperature.has_value());
        EXPECT_NEAR(rawToTemperature(16204, cal), 12.6860, 0.01);

        const json info = valueInfoOf(path);
        EXPECT_EQ(info["unit"], kCelsius);
        EXPECT_NEAR(info["valueMin"].get<double>(), 5.6850, 0.01);
        EXPECT_NEAR(info["valueMax"].get<double>(), 44.2823, 0.01);
        ASSERT_TRUE(info.contains("calibration"));
        const json &calibration = info["calibration"];
        EXPECT_DOUBLE_EQ(calibration["emissivity"].get<double>(), 0.95);
        EXPECT_FALSE(calibration.contains("reflectedApparentTemperature"));
        EXPECT_FALSE(calibration.contains("atmosphericTemperature"));
        EXPECT_FALSE(calibration.contains("irWindowTemperature"));
        EXPECT_NEAR(pointOf(path, 10, 5)["value"].get<double>(), 12.6860, 0.01);
    }

    TEST(RJpegThermal, aFlirRJpegWithoutThePlanckConstantsHasRawCountsWithoutAUnit)
    {
        TestArea ta(TEST_NAME);
        // The emissivity alone is no calibration: the Planck constants of no other camera are those of this one
        const auto path = writeFlirPicture(ta.getPath("FLIR_0002.JPG"),
                                           {{"Emissivity", "0.95"}, {"RawThermalImageWidth", "64"},
                                            {"RawThermalImageHeight", "48"}});
        EXPECT_FALSE(extractThermalCalibration(path.string()).valid);

        const json info = valueInfoOf(path);
        EXPECT_EQ(info["unit"], "");
        EXPECT_FALSE(info.contains("calibration"));
        EXPECT_EQ(info["valueMin"], 15104);
        EXPECT_EQ(info["valueMax"], 22344);
        EXPECT_EQ(pointOf(path, 10, 5)["value"], 16204);

        // A thermal formula is refused for want of the Planck constants: a FLIR picture has nothing to do with the
        // conversion of DJI
        const auto [err, message] = pointErrorOf(path, 10, 5, "CELSIUS");
        EXPECT_EQ(err, DDBERR_INVALIDARGS);
        EXPECT_NE(message.find("No thermal calibration"), std::string::npos) << message;
        EXPECT_NE(message.find("no Planck constants"), std::string::npos) << message;
        EXPECT_EQ(message.find("DJI"), std::string::npos) << message;
    }

    // ---------------------------------------------------------------- FLIR: the FFF records of the APP1 segments

    // A FLIR R-JPEG keeps its Planck constants and its raw image in the FFF data of its FLIR APP1 segments, not in its
    // XMP: the CameraInfo record (type 0x20) and the RawData record (type 1), as ExifTool documents them
    // (Image::ExifTool::FLIR). These pictures are written here with the layout of a FLIR camera (a big-endian FFF header,
    // little-endian records), for the byte orders and segmentations real files do not all show; the real FLIR pictures of
    // test_data/thermal/FLIR are read by the integration tests (thermal_test.cpp). The constants of their CameraInfo
    // record are those of a FLIR Vue Pro R (R1 16528.178, B 1427.5, F 1, O -1307, R2 0.012874875); stored as floats, they
    // are R1 16528.177734375 and R2 0.012874875217676163. The expected temperatures are literals of the FLIR measurement
    // equations (Thermimage's raw2temp: emissivity 0.95, reflected 20 C, 3 m of atmosphere at 22 C and 50 % humidity, no
    // window) computed outside the code with those floats: 15104 -> 42.3079 C, 15124 -> 42.4127 C, 15204 -> 42.8312 C,
    // 15224 -> 42.9356 C, 16204 -> 47.9446 C, 22344 -> 75.6626 C.

    struct FffSpec
    {
        bool headerBigEndian = true;      // the FFF header and its record directory
        bool cameraInfoBigEndian = false; // the CameraInfo record
        bool rawBigEndian = false;        // the RawData record
        bool pngRaw = false;              // the raw image is a PNG image (the format of the FLIR handheld cameras)
        bool withCameraInfo = true;
        float humidity = 0.5f;            // RelativeHumidity: a fraction, or a percentage
        size_t segment = 1500;            // the FFF bytes in each FLIR APP1 segment
        int rawWidth = kWidth;            // the size the RawData record declares
        int rawHeight = kHeight;
        std::vector<uint8_t> rawImage;    // when given, the bytes of the RawData record from 0x20 (a PNG image)
    };

    void putBytes(std::vector<uint8_t> &out, size_t at, uint32_t value, int size, bool bigEndian)
    {
        for (int i = 0; i < size; i++)
        {
            const int shift = 8 * (bigEndian ? size - 1 - i : i);
            out[at + static_cast<size_t>(i)] = static_cast<uint8_t>((value >> shift) & 0xFF);
        }
    }

    void putFloat(std::vector<uint8_t> &out, size_t at, float value, bool bigEndian)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        putBytes(out, at, bits, 4, bigEndian);
    }

    // The FFF data of a FLIR picture of kWidth x kHeight raw counts (flirRawAt)
    std::vector<uint8_t> fffData(const FffSpec &spec)
    {
        const size_t entries = spec.withCameraInfo ? 2 : 1;
        const size_t rawOffset = 0x40 + entries * 32;
        const size_t rawLength = 0x20 + (spec.rawImage.empty()
                                             ? static_cast<size_t>(spec.rawWidth) * spec.rawHeight * 2
                                             : spec.rawImage.size());
        const size_t cameraOffset = rawOffset + rawLength;
        const size_t cameraLength = 0x320;
        std::vector<uint8_t> fff(cameraOffset + (spec.withCameraInfo ? cameraLength : 0), 0);

        // The header: "FFF\0", the creator (16 bytes), the version, the directory and its entries
        std::memcpy(fff.data(), "FFF\0", 4);
        const bool h = spec.headerBigEndian;
        putBytes(fff, 0x14, 100, 4, h);
        putBytes(fff, 0x18, 0x40, 4, h);
        putBytes(fff, 0x1c, static_cast<uint32_t>(entries), 4, h);
        const auto entry = [&](size_t i, uint16_t type, size_t offset, size_t length) {
            const size_t at = 0x40 + i * 32;
            putBytes(fff, at, type, 2, h);
            putBytes(fff, at + 0x02, 1, 2, h);    // subtype
            putBytes(fff, at + 0x04, 0x64, 4, h); // version
            putBytes(fff, at + 0x08, 1, 4, h);    // index id
            putBytes(fff, at + 0x0c, static_cast<uint32_t>(offset), 4, h);
            putBytes(fff, at + 0x10, static_cast<uint32_t>(length), 4, h);
        };
        entry(0, 0x01, rawOffset, rawLength);
        if (spec.withCameraInfo)
            entry(1, 0x20, cameraOffset, cameraLength);

        // RawData: its byte order (2), width and height, then the raw counts from 0x20
        const bool r = spec.rawBigEndian;
        putBytes(fff, rawOffset, 2, 2, r);
        putBytes(fff, rawOffset + 0x02, static_cast<uint32_t>(spec.rawWidth), 2, r);
        putBytes(fff, rawOffset + 0x04, static_cast<uint32_t>(spec.rawHeight), 2, r);
        if (!spec.rawImage.empty())
            std::memcpy(&fff[rawOffset + 0x20], spec.rawImage.data(), spec.rawImage.size());
        else
            for (int y = 0; y < spec.rawHeight; y++)
                for (int x = 0; x < spec.rawWidth; x++)
                    putBytes(fff, rawOffset + 0x20 + (static_cast<size_t>(y) * spec.rawWidth + x) * 2,
                             flirRawAt(x % kWidth, y % kHeight), 2, r);
        if (spec.pngRaw)
        {
            const uint8_t png[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
            std::memcpy(&fff[rawOffset + 0x20], png, sizeof(png));
        }

        if (spec.withCameraInfo)
        {
            const bool c = spec.cameraInfoBigEndian;
            const size_t o = cameraOffset;
            putBytes(fff, o, 2, 2, c);
            putFloat(fff, o + 0x20, 0.95f, c);       // Emissivity
            putFloat(fff, o + 0x24, 3.0f, c);        // ObjectDistance, m
            putFloat(fff, o + 0x28, 293.15f, c);     // ReflectedApparentTemperature, K
            putFloat(fff, o + 0x2c, 295.15f, c);     // AtmosphericTemperature, K
            putFloat(fff, o + 0x30, 293.15f, c);     // IRWindowTemperature, K
            putFloat(fff, o + 0x34, 1.0f, c);        // IRWindowTransmission
            putFloat(fff, o + 0x3c, spec.humidity, c);
            putFloat(fff, o + 0x58, 16528.178f, c);  // PlanckR1
            putFloat(fff, o + 0x5c, 1427.5f, c);     // PlanckB
            putFloat(fff, o + 0x60, 1.0f, c);        // PlanckF
            putFloat(fff, o + 0x70, 0.006569f, c);   // AtmosphericTransAlpha1
            putFloat(fff, o + 0x74, 0.01262f, c);    // AtmosphericTransAlpha2
            putFloat(fff, o + 0x78, -0.002276f, c);  // AtmosphericTransBeta1
            putFloat(fff, o + 0x7c, -0.00667f, c);   // AtmosphericTransBeta2
            putFloat(fff, o + 0x80, 1.9f, c);        // AtmosphericTransX
            putBytes(fff, o + 0x308, static_cast<uint32_t>(-1307), 4, c); // PlanckO, int32s
            putFloat(fff, o + 0x30c, 0.012874875f, c);                    // PlanckR2
        }
        return fff;
    }

    // A picture of a FLIR camera with FFF records: the EXIF block, an XMP packet with the Planck constants of another
    // camera (kFlirCalibration: the CameraInfo record is what the camera says), and the FFF data in FLIR APP1 segments
    // ("FLIR\0", 0x01, the index of the segment, the index of the last one, then spec.segment bytes of the data)
    fs::path writeFlirFffPicture(const fs::path &path, const FffSpec &spec)
    {
        const auto jpeg = plainJpeg(path.string() + ".tmp.jpg");
        std::vector<uint8_t> out = {0xFF, 0xD8};
        const auto exif = exifSegment("FLIR Systems AB", "FLIR Vue Pro R");
        out.insert(out.end(), exif.begin(), exif.end());

        std::string xmp = std::string("http://ns.adobe.com/xap/1.0/") + '\0' +
                          "<?xpacket begin='' id='W5M0MpCehiHzreSzNTczkc9d'?>"
                          "<x:xmpmeta xmlns:x='adobe:ns:meta/'><rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>"
                          "<rdf:Description rdf:about='' xmlns:FLIR='http://ns.flir.com/xmp/1.0/'";
        for (const auto &[name, value] : kFlirCalibration)
            xmp += " FLIR:" + name + "='" + value + "'";
        xmp += "/></rdf:RDF></x:xmpmeta><?xpacket end='w'?>";
        const auto xmpSegment = app1(xmp);
        out.insert(out.end(), xmpSegment.begin(), xmpSegment.end());

        const std::vector<uint8_t> fff = fffData(spec);
        const size_t segments = (fff.size() + spec.segment - 1) / spec.segment;
        for (size_t i = 0; i < segments; i++)
        {
            std::string payload("FLIR\0\x01", 6);
            payload.push_back(static_cast<char>(i));
            payload.push_back(static_cast<char>(segments - 1));
            const size_t at = i * spec.segment;
            payload.append(reinterpret_cast<const char *>(fff.data() + at), std::min(spec.segment, fff.size() - at));
            const auto segment = app1(payload);
            out.insert(out.end(), segment.begin(), segment.end());
        }
        out.insert(out.end(), jpeg.begin() + 2, jpeg.end());

        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char *>(out.data()), static_cast<std::streamsize>(out.size()));
        return path;
    }

    void expectVueProCalibration(const ThermalCalibration &cal)
    {
        ASSERT_TRUE(cal.valid);
        EXPECT_DOUBLE_EQ(cal.planckR1, 16528.177734375);
        EXPECT_DOUBLE_EQ(cal.planckB, 1427.5);
        EXPECT_DOUBLE_EQ(cal.planckF, 1.0);
        EXPECT_DOUBLE_EQ(cal.planckO, -1307.0);
        EXPECT_DOUBLE_EQ(cal.planckR2, 0.012874875217676163);
        ASSERT_TRUE(cal.emissivity && cal.objectDistance && cal.reflectedApparentTemperature &&
                    cal.atmosphericTemperature && cal.irWindowTemperature && cal.irWindowTransmission &&
                    cal.relativeHumidity);
        EXPECT_NEAR(*cal.emissivity, 0.95, 1e-6);
        EXPECT_NEAR(*cal.objectDistance, 3.0, 1e-6);
        // 293.15 K and 295.15 K in degrees Celsius
        EXPECT_NEAR(*cal.reflectedApparentTemperature, 20.0, 1e-4);
        EXPECT_NEAR(*cal.atmosphericTemperature, 22.0, 1e-4);
        EXPECT_NEAR(*cal.irWindowTemperature, 20.0, 1e-4);
        EXPECT_NEAR(*cal.irWindowTransmission, 1.0, 1e-6);
        EXPECT_NEAR(*cal.relativeHumidity, 0.5, 1e-6);
        ASSERT_TRUE(cal.atmosphericTransAlpha1 && cal.atmosphericTransAlpha2 && cal.atmosphericTransBeta1 &&
                    cal.atmosphericTransBeta2 && cal.atmosphericTransX);
        EXPECT_NEAR(*cal.atmosphericTransAlpha1, 0.006569, 1e-7);
        EXPECT_NEAR(*cal.atmosphericTransAlpha2, 0.01262, 1e-7);
        EXPECT_NEAR(*cal.atmosphericTransBeta1, -0.002276, 1e-7);
        EXPECT_NEAR(*cal.atmosphericTransBeta2, -0.00667, 1e-7);
        EXPECT_NEAR(*cal.atmosphericTransX, 1.9, 1e-6);
    }

    TEST(RJpegThermal, theCalibrationOfAFlirRJpegIsTheOneOfItsCameraInfoRecord)
    {
        TestArea ta(TEST_NAME);
        // The layout of a FLIR R-JPEG (a big-endian header, a little-endian record), a big-endian record, and the
        // little-endian header of an FFF file; the humidity as a fraction or as a percentage
        FffSpec flir;
        FffSpec bigEndianRecord;
        bigEndianRecord.cameraInfoBigEndian = true;
        FffSpec littleEndianHeader;
        littleEndianHeader.headerBigEndian = false;
        littleEndianHeader.humidity = 50.0f;
        int n = 0;
        for (const FffSpec &spec : {flir, bigEndianRecord, littleEndianHeader})
        {
            SCOPED_TRACE(n);
            const auto path = writeFlirFffPicture(ta.getPath("FLIR_FFF_" + std::to_string(n++) + ".JPG"), spec);
            // Not the constants of the XMP (R1 21106.77): those of the camera, in its CameraInfo record
            expectVueProCalibration(extractThermalCalibration(path.string()));
        }
    }

    TEST(RJpegThermal, theRawImageOfAFlirRJpegIsTheOneOfItsRawDataRecord)
    {
        TestArea ta(TEST_NAME);
        // The FFF data spans 6 APP1 segments of 1500 bytes, the raw image several of them
        FffSpec flir;
        FffSpec bigEndianRaw;
        bigEndianRaw.rawBigEndian = true;
        int n = 0;
        for (const FffSpec &spec : {flir, bigEndianRaw})
        {
            SCOPED_TRACE(n);
            const auto path = writeFlirFffPicture(ta.getPath("FLIR_RAW_" + std::to_string(n++) + ".JPG"), spec);
            EXPECT_TRUE(isThermalImage(path.string()));
            const RawThermalData raw = extractRawThermalData(path.string());
            ASSERT_TRUE(raw.valid);
            EXPECT_EQ(raw.width, kWidth);
            EXPECT_EQ(raw.height, kHeight);
            ASSERT_EQ(raw.data.size(), static_cast<size_t>(kWidth) * kHeight);
            EXPECT_EQ(raw.data[0], 15104);
            EXPECT_EQ(raw.data[10 + 5 * kWidth], 16204);
            EXPECT_EQ(raw.data[63 + 47 * kWidth], 22344);
        }
    }

    TEST(RJpegThermal, theRasterApiOfAFlirRJpegGivesTheTemperaturesOfItsCameraInfoRecord)
    {
        TestArea ta(TEST_NAME);
        const auto path = writeFlirFffPicture(ta.getPath("FLIR_0010.JPG"), FffSpec());

        const json info = valueInfoOf(path);
        EXPECT_EQ(info["width"], kWidth);
        EXPECT_EQ(info["height"], kHeight);
        EXPECT_EQ(info["unit"], kCelsius);
        EXPECT_TRUE(info["isThermal"].get<bool>());
        EXPECT_NEAR(info["valueMin"].get<double>(), 42.3079, 0.01);
        EXPECT_NEAR(info["valueMax"].get<double>(), 75.6626, 0.01);
        ASSERT_TRUE(info.contains("calibration"));
        const json &calibration = info["calibration"];
        EXPECT_NEAR(calibration["planckR1"].get<double>(), 16528.1777, 1e-3);
        EXPECT_NEAR(calibration["planckO"].get<double>(), -1307.0, 1e-9);
        EXPECT_NEAR(calibration["reflectedApparentTemperature"].get<double>(), 20.0, 1e-4);
        EXPECT_NEAR(calibration["relativeHumidity"].get<double>(), 0.5, 1e-6);
        EXPECT_NEAR(calibration["atmosphericTransX"].get<double>(), 1.9, 1e-6);

        const json point = pointOf(path, 10, 5);
        EXPECT_NEAR(point["value"].get<double>(), 47.9446, 0.01);
        EXPECT_EQ(point["rawValue"], 16204);
        EXPECT_EQ(point["unit"], kCelsius);
        const json kelvin = pointOf(path, 63, 47, "KELVIN");
        EXPECT_EQ(kelvin["unit"], "K");
        EXPECT_NEAR(kelvin["value"].get<double>(), 348.8126, 0.01);

        char *output = nullptr;
        ASSERT_EQ(DDBGetRasterAreaStatsPixelEx(path.string().c_str(), 0, 0, 1, 1, nullptr, &output), DDBERR_NONE)
            << DDBGetLastError();
        const json area = json::parse(std::string(output));
        DDBFree(output);
        // 15104, 15204, 15124, 15224
        EXPECT_EQ(area["pixelCount"], 4);
        EXPECT_NEAR(area["min"].get<double>(), 42.3079, 0.01);
        EXPECT_NEAR(area["max"].get<double>(), 42.9356, 0.01);
        EXPECT_NEAR(area["mean"].get<double>(), 42.6218, 0.01);
    }

    TEST(RJpegThermal, aFlirRawImageInAPngImageThatCannotBeDecodedIsNotTakenForRawCounts)
    {
        TestArea ta(TEST_NAME);
        // The raw image of the RawData record starts as a PNG image and is not one (the signature, then raw counts): no
        // raw count is read from the bytes of the record (nor from those of the APP1 segments), and the picture is its
        // colour preview. The PNG images of real cameras are read by the integration tests (thermal_test.cpp)
        FffSpec spec;
        spec.pngRaw = true;
        const auto path = writeFlirFffPicture(ta.getPath("FLIR_0011.JPG"), spec);

        EXPECT_TRUE(isThermalImage(path.string()));
        EXPECT_FALSE(extractRawThermalData(path.string()).valid);
        EXPECT_FALSE(readRJpegThermal(path.string()).has_value());

        const json info = valueInfoOf(path);
        EXPECT_TRUE(info["isThermal"].get<bool>());
        EXPECT_EQ(info["unit"], "");
        EXPECT_FALSE(info.contains("calibration"));
        EXPECT_EQ(pointOf(path, 10, 5)["value"], kPreviewGrey);

        // The reason of the refusal is the colour preview, not raw counts nor DJI's conversion
        const auto [err, message] = pointErrorOf(path, 10, 5, "CELSIUS");
        EXPECT_EQ(err, DDBERR_INVALIDARGS);
        EXPECT_NE(message.find("No thermal calibration"), std::string::npos) << message;
        EXPECT_NE(message.find("colours of its preview"), std::string::npos) << message;
        EXPECT_EQ(message.find("DJI"), std::string::npos) << message;
    }

    // ---------------------------------------------------------------- FLIR: the PNG image of a RawData record

    // The PNG images of the RawData records are written here byte by byte, without zlib: the CRC-32 of the chunks, the
    // zlib stream (stored blocks, or fixed Huffman codes for a long run of zeros) and the five row filters of the PNG
    // format, so that the decoder is checked against an encoder that shares no code with it. The real PNG images of
    // the FLIR handheld cameras (AX8, FLIR ONE) are read by the integration tests (thermal_test.cpp)

    // A raw count of the PNG images: not a plane, so that the predictors of the filters differ from row to row
    uint16_t pngRawAt(int x, int y)
    {
        return static_cast<uint16_t>(15104 + 100 * x + 20 * y + ((x * 7 + y * 13) % 37) * 3);
    }

    void putBigEndian32(std::vector<uint8_t> &out, uint32_t value)
    {
        for (int i = 3; i >= 0; i--)
            out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
    }

    // The CRC-32 of PNG chunks (ISO 3309, the polynomial 0xEDB88320, bit by bit)
    uint32_t crc32Of(const std::vector<uint8_t> &data)
    {
        uint32_t crc = 0xFFFFFFFFu;
        for (const uint8_t byte : data)
        {
            crc ^= byte;
            for (int k = 0; k < 8; k++)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return crc ^ 0xFFFFFFFFu;
    }

    uint32_t adler32Of(const std::vector<uint8_t> &data)
    {
        uint32_t a = 1;
        uint32_t b = 0;
        for (const uint8_t byte : data)
        {
            a = (a + byte) % 65521;
            b = (b + a) % 65521;
        }
        return (b << 16) | a;
    }

    // A zlib stream of the data in stored blocks (no compression)
    std::vector<uint8_t> zlibStored(const std::vector<uint8_t> &data)
    {
        std::vector<uint8_t> out = {0x78, 0x01};
        size_t at = 0;
        do
        {
            const size_t n = std::min<size_t>(65535, data.size() - at);
            out.push_back(at + n == data.size() ? 1 : 0); // BFINAL, BTYPE 00
            out.push_back(static_cast<uint8_t>(n & 0xFF));
            out.push_back(static_cast<uint8_t>(n >> 8));
            out.push_back(static_cast<uint8_t>(~n & 0xFF));
            out.push_back(static_cast<uint8_t>((~n >> 8) & 0xFF));
            out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(at),
                       data.begin() + static_cast<std::ptrdiff_t>(at + n));
            at += n;
        } while (at < data.size());
        putBigEndian32(out, adler32Of(data));
        return out;
    }

    // A zlib stream of count zero bytes, small: one block of fixed Huffman codes, a literal 0 then matches of 258 bytes
    // at the distance 1 (13 bits each), as a file that declares a large image with little data would hold
    std::vector<uint8_t> zlibZeros(size_t count)
    {
        std::vector<uint8_t> out = {0x78, 0x01};
        uint32_t pending = 0;
        int used = 0;
        const auto bit = [&](uint32_t b) {
            pending |= (b & 1u) << used;
            if (++used == 8)
            {
                out.push_back(static_cast<uint8_t>(pending));
                pending = 0;
                used = 0;
            }
        };
        // A Huffman code goes most significant bit first
        const auto code = [&](uint32_t c, int length) {
            for (int i = length - 1; i >= 0; i--)
                bit(c >> i);
        };
        bit(1); // BFINAL
        bit(1); // BTYPE 01 (fixed codes), least significant bit first
        bit(0);
        size_t left = count;
        if (left > 0)
        {
            code(0x30, 8); // the literal 0
            left--;
        }
        for (; left >= 258; left -= 258)
        {
            code(0xC5, 8); // the length 258 (code 285)
            code(0, 5);    // the distance 1 (code 0)
        }
        for (; left > 0; left--)
            code(0x30, 8);
        code(0, 7); // the end of the block (256)
        if (used > 0)
            out.push_back(static_cast<uint8_t>(pending));
        // The Adler-32 of zeros: a = 1, b = count
        putBigEndian32(out, static_cast<uint32_t>((count % 65521) << 16) | 1u);
        return out;
    }

    struct PngChunk
    {
        std::string type;
        std::vector<uint8_t> data;
        bool goodCrc = true;
    };

    // A PNG image: the signature, then the chunks with their length and their CRC
    std::vector<uint8_t> pngOf(const std::vector<PngChunk> &chunks)
    {
        std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        for (const PngChunk &chunk : chunks)
        {
            putBigEndian32(png, static_cast<uint32_t>(chunk.data.size()));
            std::vector<uint8_t> typed(chunk.type.begin(), chunk.type.end());
            typed.insert(typed.end(), chunk.data.begin(), chunk.data.end());
            png.insert(png.end(), typed.begin(), typed.end());
            putBigEndian32(png, crc32Of(typed) ^ (chunk.goodCrc ? 0u : 1u));
        }
        return png;
    }

    // IHDR: the size, the bit depth, the colour type (0: greyscale), then compression, filter and interlace
    std::vector<uint8_t> ihdrOf(uint32_t width, uint32_t height, uint8_t depth = 16, uint8_t colourType = 0,
                                uint8_t interlace = 0)
    {
        std::vector<uint8_t> data;
        putBigEndian32(data, width);
        putBigEndian32(data, height);
        data.insert(data.end(), {depth, colourType, 0, 0, interlace});
        return data;
    }

    // The rows of the raw image pngRawAt as a PNG encoder writes them: the filter of the row y is y % 5 (None, Sub, Up,
    // Average, Paeth; PNG 9.2), the samples in the byte order of the record
    std::vector<uint8_t> filteredRows(int width, int height, bool bigEndian)
    {
        const size_t rowBytes = static_cast<size_t>(width) * 2;
        std::vector<uint8_t> previous(rowBytes, 0);
        std::vector<uint8_t> out;
        for (int y = 0; y < height; y++)
        {
            std::vector<uint8_t> line(rowBytes);
            for (int x = 0; x < width; x++)
            {
                const uint16_t v = pngRawAt(x, y);
                line[static_cast<size_t>(x) * 2] = static_cast<uint8_t>(bigEndian ? v >> 8 : v & 0xFF);
                line[static_cast<size_t>(x) * 2 + 1] = static_cast<uint8_t>(bigEndian ? v & 0xFF : v >> 8);
            }
            const int filter = y % 5;
            out.push_back(static_cast<uint8_t>(filter));
            for (size_t i = 0; i < rowBytes; i++)
            {
                const int a = i >= 2 ? line[i - 2] : 0; // left
                const int b = previous[i];              // up
                const int c = i >= 2 ? previous[i - 2] : 0; // up left
                int predictor = 0;
                if (filter == 1)
                    predictor = a;
                else if (filter == 2)
                    predictor = b;
                else if (filter == 3)
                    predictor = (a + b) / 2;
                else if (filter == 4)
                {
                    const int p = a + b - c;
                    const int pa = std::abs(p - a);
                    const int pb = std::abs(p - b);
                    const int pc = std::abs(p - c);
                    predictor = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                }
                out.push_back(static_cast<uint8_t>(line[i] - predictor));
            }
            previous = line;
        }
        return out;
    }

    // A FLIR picture whose RawData record (rawWidth x rawHeight, in its byte order) holds the PNG image
    fs::path writeFlirPngPicture(const fs::path &path, const std::vector<uint8_t> &png, bool bigEndian = false,
                                 int rawWidth = kWidth, int rawHeight = kHeight)
    {
        FffSpec spec;
        spec.rawBigEndian = bigEndian;
        spec.rawImage = png;
        spec.rawWidth = rawWidth;
        spec.rawHeight = rawHeight;
        spec.segment = 60000;
        return writeFlirFffPicture(path, spec);
    }

    TEST(RJpegThermal, theRawImageOfAFlirPngIsDecodedWithEveryRowFilterInTheByteOrderOfTheRecord)
    {
        TestArea ta(TEST_NAME);
        // A little-endian record (the handheld cameras: the samples byte-swapped in the PNG image) in one IDAT chunk, a
        // big-endian one (the old ThermaCAM: the samples in PNG order, not swapped) in two
        for (const bool bigEndian : {false, true})
        {
            SCOPED_TRACE(bigEndian ? "big-endian record" : "little-endian record");
            const auto stream = zlibStored(filteredRows(kWidth, kHeight, bigEndian));
            std::vector<PngChunk> chunks = {{"IHDR", ihdrOf(kWidth, kHeight)}};
            if (bigEndian)
            {
                const size_t half = stream.size() / 2;
                chunks.push_back({"IDAT", std::vector<uint8_t>(stream.begin(), stream.begin() + half)});
                chunks.push_back({"IDAT", std::vector<uint8_t>(stream.begin() + half, stream.end())});
            }
            else
            {
                chunks.push_back({"IDAT", stream});
            }
            chunks.push_back({"IEND", {}});
            const auto path =
                writeFlirPngPicture(ta.getPath(bigEndian ? "FLIR_PNG_BE.JPG" : "FLIR_PNG_LE.JPG"), pngOf(chunks), bigEndian);

            const RawThermalData raw = extractRawThermalData(path.string());
            ASSERT_TRUE(raw.valid);
            EXPECT_EQ(raw.width, kWidth);
            EXPECT_EQ(raw.height, kHeight);
            ASSERT_EQ(raw.data.size(), static_cast<size_t>(kWidth) * kHeight);
            int wrong = 0;
            for (int y = 0; y < kHeight; y++)
                for (int x = 0; x < kWidth; x++)
                    if (raw.data[static_cast<size_t>(y) * kWidth + x] != pngRawAt(x, y) && wrong++ < 5)
                        ADD_FAILURE() << "the pixel (" << x << ", " << y << ") of a row with the filter " << y % 5
                                      << " is " << raw.data[static_cast<size_t>(y) * kWidth + x] << ", not "
                                      << pngRawAt(x, y);
            EXPECT_EQ(wrong, 0);
            // The raster API reads the same counts
            EXPECT_EQ(pointOf(path, 10, 9)["rawValue"], pngRawAt(10, 9));
        }
    }

    TEST(RJpegThermal, aFlirPngThatIsNotTheImageFlirWritesIsNoRawImage)
    {
        TestArea ta(TEST_NAME);
        const std::vector<uint8_t> rows = filteredRows(kWidth, kHeight, false);
        const std::vector<uint8_t> stream = zlibStored(rows);
        const PngChunk ihdr = {"IHDR", ihdrOf(kWidth, kHeight)};
        const PngChunk idat = {"IDAT", stream};
        const PngChunk iend = {"IEND", {}};

        // The image itself is read: what the cases below change is what makes them no raw image
        const auto good = writeFlirPngPicture(ta.getPath("FLIR_PNG_GOOD.JPG"), pngOf({ihdr, idat, iend}));
        ASSERT_TRUE(extractRawThermalData(good.string()).valid);

        std::vector<uint8_t> moreRows = rows;
        moreRows.insert(moreRows.end(), rows.begin(), rows.begin() + 1 + kWidth * 2); // one row too many
        const std::vector<uint8_t> truncated(stream.begin(), stream.end() - 8);       // the end of the data is missing
        const std::vector<uint8_t> shortRows(rows.begin(), rows.end() - (1 + kWidth * 2));

        struct Case
        {
            const char *name;
            std::vector<PngChunk> chunks;
            int rawHeight = kHeight;
        };
        const std::vector<Case> cases = {
            {"a chunk with a wrong CRC", {ihdr, {"IDAT", stream, false}, iend}},
            {"a truncated IDAT", {ihdr, {"IDAT", truncated}, iend}},
            {"IDAT data that inflates to more than the rows", {ihdr, {"IDAT", zlibStored(moreRows)}, iend}},
            {"IDAT data far too short for its rows (deflate gives at most 1032 bytes a byte)",
             {ihdr, {"IDAT", {0x78, 0x01, 0x03, 0x00}}, iend}},
            {"IHDR after IDAT", {idat, ihdr, iend}},
            {"two IHDR", {ihdr, ihdr, idat, iend}},
            {"no IEND", {ihdr, idat}},
            {"a PLTE chunk", {ihdr, {"PLTE", {0, 0, 0, 255, 255, 255}}, idat, iend}},
            {"an 8-bit image", {{"IHDR", ihdrOf(kWidth, kHeight, 8)}, idat, iend}},
            {"a colour image", {{"IHDR", ihdrOf(kWidth, kHeight, 16, 2)}, idat, iend}},
            {"an interlaced image", {{"IHDR", ihdrOf(kWidth, kHeight, 16, 0, 1)}, idat, iend}},
            {"an image of another size than the record", {{"IHDR", ihdrOf(kWidth, kHeight - 1)},
                                                          {"IDAT", zlibStored(shortRows)}, iend}},
            {"a record of another size than the image", {ihdr, idat, iend}, kHeight + 1},
        };
        int n = 0;
        for (const Case &c : cases)
        {
            SCOPED_TRACE(c.name);
            const auto path = writeFlirPngPicture(ta.getPath("FLIR_PNG_" + std::to_string(n++) + ".JPG"),
                                                  pngOf(c.chunks), false, kWidth, c.rawHeight);
            // No raw count from the PNG image, nor from the bytes of the record or of the APP1 segments
            EXPECT_TRUE(isThermalImage(path.string()));
            EXPECT_FALSE(extractRawThermalData(path.string()).valid);
            EXPECT_FALSE(readRJpegThermal(path.string()).has_value());
        }
    }

    // A small file must not make a request allocate gigabytes: the size a RawData record declares is checked against
    // kMaxRawThermalPixels (4 MP, over three times the 1280 x 1024 of the largest radiometric sensors) before anything is
    // allocated for it, whether the record holds int16u counts or a PNG image. A PNG image of zeros at the bound is some
    // 50 KB: one pixel row more is refused, at the bound it is read
    TEST(RJpegThermal, theRawImageOfAFlirRecordIsReadUpToFourMegapixels)
    {
        TestArea ta(TEST_NAME);
        static_assert(kMaxRawThermalPixels == static_cast<size_t>(2048) * 2048, "the bound of a raw thermal image");
        EXPECT_GE(kMaxRawThermalPixels, static_cast<size_t>(1280) * 1024);
        EXPECT_GE(kMaxRawThermalPixels, static_cast<size_t>(2048) * 1536);

        for (const int height : {2048, 2049})
        {
            SCOPED_TRACE(height);
            const bool fits = static_cast<size_t>(2048) * height <= kMaxRawThermalPixels;
            EXPECT_EQ(fits, height == 2048);

            // A PNG image of zeros (the filter byte of each row is 0, None)
            const size_t rowsSize = static_cast<size_t>(height) * (1 + 2048 * 2);
            const auto png = pngOf({{"IHDR", ihdrOf(2048, static_cast<uint32_t>(height))},
                                    {"IDAT", zlibZeros(rowsSize)},
                                    {"IEND", {}}});
            EXPECT_LT(png.size(), static_cast<size_t>(60000));
            const auto pngPath =
                writeFlirPngPicture(ta.getPath("FLIR_PNG_" + std::to_string(height) + ".JPG"), png, false, 2048, height);
            const RawThermalData fromPng = extractRawThermalData(pngPath.string());
            EXPECT_EQ(fromPng.valid, fits);
            if (fits)
            {
                EXPECT_EQ(fromPng.width, 2048);
                EXPECT_EQ(fromPng.height, height);
                ASSERT_EQ(fromPng.data.size(), kMaxRawThermalPixels);
                EXPECT_EQ(fromPng.data.front(), 0);
                EXPECT_EQ(fromPng.data.back(), 0);
            }

            // The int16u counts of the same size, whole in the record
            FffSpec spec;
            spec.rawWidth = 2048;
            spec.rawHeight = height;
            spec.segment = 65000;
            const auto countsPath = writeFlirFffPicture(ta.getPath("FLIR_RAW_" + std::to_string(height) + ".JPG"), spec);
            const RawThermalData fromCounts = extractRawThermalData(countsPath.string());
            EXPECT_EQ(fromCounts.valid, fits);
            if (fits)
            {
                ASSERT_EQ(fromCounts.data.size(), kMaxRawThermalPixels);
                EXPECT_EQ(fromCounts.data[0], 15104);
                EXPECT_EQ(fromCounts.data[10 + 5 * 2048], 16204);
            }
        }
    }

    // ---------------------------------------------------------------- The PNG decoder on its own

    // The bound of the decoder itself, not only the one fffRawThermal checks first: a PNG image of 2048 x 2049 zeros is
    // refused before anything is inflated, and the same image of 2048 x 2048 is read; a size that is not positive is
    // no image
    TEST(RJpegThermal, theFlirPngDecoderReadsAnImageUpToFourMegapixelsItself)
    {
        for (const int height : {2048, 2049})
        {
            SCOPED_TRACE(height);
            const size_t rowsSize = static_cast<size_t>(height) * (1 + 2048 * 2);
            const auto png = pngOf({{"IHDR", ihdrOf(2048, static_cast<uint32_t>(height))},
                                    {"IDAT", zlibZeros(rowsSize)},
                                    {"IEND", {}}});
            const uint64_t inflates = thermalReadCountersForTesting().pngInflates;
            const auto counts = decodeFlirPng(png.data(), png.size(), 2048, height, false);
            if (height == 2048)
            {
                ASSERT_TRUE(counts.has_value());
                ASSERT_EQ(counts->size(), kMaxRawThermalPixels);
                EXPECT_EQ(std::count(counts->begin(), counts->end(), uint16_t(0)),
                          static_cast<std::ptrdiff_t>(kMaxRawThermalPixels));
                EXPECT_EQ(thermalReadCountersForTesting().pngInflates, inflates + 1);
            }
            else
            {
                EXPECT_FALSE(counts.has_value());
                EXPECT_EQ(thermalReadCountersForTesting().pngInflates, inflates);
            }
        }

        const auto png = pngOf({{"IHDR", ihdrOf(kWidth, kHeight)},
                                {"IDAT", zlibStored(filteredRows(kWidth, kHeight, false))},
                                {"IEND", {}}});
        ASSERT_TRUE(decodeFlirPng(png.data(), png.size(), kWidth, kHeight, false).has_value());
        const uint64_t inflates = thermalReadCountersForTesting().pngInflates;
        for (const auto &[width, height] : std::vector<std::pair<int, int>>{{0, kHeight}, {kWidth, 0}, {-kWidth, kHeight},
                                                                           {kWidth, -kHeight}, {-kWidth, -kHeight}})
        {
            SCOPED_TRACE(std::to_string(width) + " x " + std::to_string(height));
            EXPECT_FALSE(decodeFlirPng(png.data(), png.size(), width, height, false).has_value());
        }
        EXPECT_EQ(thermalReadCountersForTesting().pngInflates, inflates);
    }

    // IDAT data that cannot inflate to the rows is refused before they are allocated: deflate gives at most 1032 bytes a
    // byte. The streams here are whole zlib streams that zlib inflates (stored blocks of zeros, 11 bytes more than their
    // data: each is read as the image whose rows it holds), too short for the rows of a 2048 x 2048 image (8390656
    // bytes, 8130.5 times 1032): the one of 8130 bytes is refused by the ratio, without inflating, and the one of 8131
    // bytes is inflated, and refused because it does not give the rows
    TEST(RJpegThermal, idatDataTooShortForTheRowsIsRefusedByTheDeflateRatioBeforeInflating)
    {
        const size_t rowsSize = static_cast<size_t>(2048) * (1 + 2048 * 2);
        ASSERT_EQ(rowsSize, static_cast<size_t>(8390656));

        struct Case
        {
            size_t zeros;      // the data of the stream
            int width, height; // the image its data are the rows of
            bool ratioPasses;  // stream size * 1032 >= the rows of 2048 x 2048
        };
        // 8119 = 23 rows of 1 + 176 * 2 bytes, 8120 = 8 rows of 1 + 507 * 2 bytes
        for (const Case &c : {Case{8119, 176, 23, false}, Case{8120, 507, 8, true}})
        {
            SCOPED_TRACE(c.zeros);
            const std::vector<uint8_t> stream = zlibStored(std::vector<uint8_t>(c.zeros, 0));
            ASSERT_EQ(stream.size(), c.zeros + 11);
            EXPECT_EQ(static_cast<uint64_t>(stream.size()) * 1032 >= rowsSize, c.ratioPasses);

            // zlib inflates the stream: as the image whose rows it holds, it is read
            const auto own = pngOf({{"IHDR", ihdrOf(static_cast<uint32_t>(c.width), static_cast<uint32_t>(c.height))},
                                    {"IDAT", stream},
                                    {"IEND", {}}});
            const auto counts = decodeFlirPng(own.data(), own.size(), c.width, c.height, false);
            ASSERT_TRUE(counts.has_value());
            EXPECT_EQ(counts->size(), static_cast<size_t>(c.width) * c.height);

            // As the rows of 2048 x 2048 pixels it is refused, by the ratio before inflating, or by inflate
            const auto large = pngOf({{"IHDR", ihdrOf(2048, 2048)}, {"IDAT", stream}, {"IEND", {}}});
            const uint64_t inflates = thermalReadCountersForTesting().pngInflates;
            EXPECT_FALSE(decodeFlirPng(large.data(), large.size(), 2048, 2048, false).has_value());
            EXPECT_EQ(thermalReadCountersForTesting().pngInflates, inflates + (c.ratioPasses ? 1 : 0));
        }
    }

    // ---------------------------------------------------------------- The marker walk of the R-JPEG reader

    // The segments of a JPEG are walked marker by marker up to the scan (walkJpegSegments): one walker for the FFF data
    // of FLIR (readFlirFffData), the APP3 segments of DJI and the FLIR segment without FFF records. The JPEGs here are
    // written byte by byte: SOI, the bytes given, a scan header (SOS), the bytes after it and EOI; no image

    std::vector<uint8_t> join(const std::vector<std::vector<uint8_t>> &parts)
    {
        std::vector<uint8_t> out;
        for (const auto &part : parts)
            out.insert(out.end(), part.begin(), part.end());
        return out;
    }

    fs::path writeJpegBytes(const fs::path &path, const std::vector<uint8_t> &body,
                            const std::vector<uint8_t> &afterScan = {})
    {
        const std::vector<uint8_t> sos = {0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00};
        const std::vector<uint8_t> out = join({{0xFF, 0xD8}, body, sos, afterScan, {0xFF, 0xD9}});
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char *>(out.data()), static_cast<std::streamsize>(out.size()));
        return path;
    }

    // A segment: the marker, the length (payload + 2, or the one given) and the payload
    std::vector<uint8_t> segmentOf(uint8_t marker, const std::vector<uint8_t> &payload, int length = -1)
    {
        const size_t declared = length < 0 ? payload.size() + 2 : static_cast<size_t>(length);
        return join({{0xFF, marker, static_cast<uint8_t>(declared >> 8), static_cast<uint8_t>(declared & 0xFF)}, payload});
    }

    // A FLIR APP1 segment of an FFF file: "FLIR\0", 0x01, its index, the index of the last one, then its data
    std::vector<uint8_t> flirPart(int index, int last, const std::string &data)
    {
        std::vector<uint8_t> payload = {'F', 'L', 'I', 'R', 0x00, 0x01, static_cast<uint8_t>(index),
                                        static_cast<uint8_t>(last)};
        payload.insert(payload.end(), data.begin(), data.end());
        return segmentOf(0xE1, payload);
    }

    std::string fffDataOf(const fs::path &path)
    {
        const std::vector<uint8_t> fff = readFlirFffData(path.string());
        return std::string(fff.begin(), fff.end());
    }

    TEST(RJpegThermal, theFffDataIsReadFromTheFlirSegmentsTheMarkerWalkReaches)
    {
        TestArea ta(TEST_NAME);
        const auto part0 = flirPart(0, 1, "AB");
        const auto part1 = flirPart(1, 1, "CD");
        const auto comment = segmentOf(0xFE, {'x', 'y', 'z'});

        struct Case
        {
            const char *name;
            std::vector<uint8_t> body;
            std::vector<uint8_t> afterScan;
            std::string fff;
        };
        const std::vector<Case> cases = {
            {"two parts in order", join({comment, part0, part1}), {}, "ABCD"},
            {"fill bytes before the markers", join({{0xFF, 0xFF}, comment, {0xFF, 0xFF, 0xFF}, part0, {0xFF}, part1}), {},
             "ABCD"},
            {"RSTn, TEM and SOI markers between the segments (no length)",
             join({{0xFF, 0xD0}, part0, {0xFF, 0x01, 0xFF, 0xD7, 0xFF, 0xD8}, part1}), {}, "ABCD"},
            {"the parts out of order", join({part1, comment, part0}), {}, "ABCD"},
            {"an index seen twice: the first is kept", join({part0, flirPart(0, 1, "XX"), part1}), {}, "ABCD"},
            {"a segment of another FFF file (another last index)", join({part0, flirPart(0, 2, "YY"), part1}), {},
             "ABCD"},
            {"an index after the last one", join({part0, flirPart(5, 1, "ZZ"), part1}), {}, "ABCD"},
            {"a segment that is not FLIR's", join({part0, segmentOf(0xE1, {'E', 'x', 'i', 'f', 0, 0}), part1}), {},
             "ABCD"},
            {"a missing part", join({flirPart(0, 2, "AB"), flirPart(2, 2, "EF")}), {}, ""},
            {"a length below 2 before the last part", join({part0, segmentOf(0xE1, {}, 1), part1}), {}, ""},
            {"a byte that is no marker before the last part", join({part0, {0x00}, part1}), {}, ""},
            {"the last part past the end of the file", join({part0, segmentOf(0xE1, {'F', 'L', 'I', 'R', 0, 1, 1, 1}, 60000)}),
             {}, ""},
            {"the end of the image before the last part", join({part0, {0xFF, 0xD9}, part1}), {}, ""},
            {"the last part after the scan", part0, part1, ""},
            {"a segment past the end of the file after the parts", join({part0, part1, segmentOf(0xFE, {}, 60000)}), {},
             "ABCD"},
            {"no FLIR segment", comment, {}, ""},
        };
        int n = 0;
        for (const Case &c : cases)
        {
            SCOPED_TRACE(c.name);
            const auto path = writeJpegBytes(ta.getPath("FFF_" + std::to_string(n++) + ".JPG"), c.body, c.afterScan);
            EXPECT_EQ(fffDataOf(path), c.fff);
        }

        // A file that is no JPEG has none, whatever it holds
        const auto notJpeg = ta.getPath("FFF_NOT_JPEG.bin");
        {
            const auto bytes = join({{0x00, 0xD8}, part0, part1});
            std::ofstream file(notJpeg, std::ios::binary);
            file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        EXPECT_EQ(fffDataOf(notJpeg), "");
    }

    // A crafted file cannot make the walk take tens of millions of steps: at most kMaxJpegMarkers markers (4096, over 60
    // times the markers real R-JPEGs have before their scan) and kMaxJpegFillBytes fill bytes (64 KB, none in real ones)
    // are walked; what is after them is not read
    TEST(RJpegThermal, theMarkerWalkStopsAfter4096MarkersAnd64KBOfFillBytes)
    {
        TestArea ta(TEST_NAME);
        static_assert(kMaxJpegMarkers == 4096, "the markers walked");
        static_assert(kMaxJpegFillBytes == 65536, "the fill bytes walked");
        const auto parts = join({flirPart(0, 1, "AB"), flirPart(1, 1, "CD")});

        // RST markers before the two FLIR segments: these are the markers 4095 and 4096 after 4094 of them
        for (const size_t rst : {static_cast<size_t>(4094), static_cast<size_t>(4095)})
        {
            SCOPED_TRACE(rst);
            std::vector<uint8_t> body;
            for (size_t i = 0; i < rst; i++)
                body.insert(body.end(), {0xFF, static_cast<uint8_t>(0xD0 + i % 8)});
            body.insert(body.end(), parts.begin(), parts.end());
            const auto path = writeJpegBytes(ta.getPath("RST_" + std::to_string(rst) + ".JPG"), body);
            EXPECT_EQ(fffDataOf(path), rst + 2 <= kMaxJpegMarkers ? "ABCD" : "");
        }

        // Fill bytes before the first FLIR segment
        for (const size_t fill : {kMaxJpegFillBytes, kMaxJpegFillBytes + 1})
        {
            SCOPED_TRACE(fill);
            std::vector<uint8_t> body(fill, 0xFF);
            body.insert(body.end(), parts.begin(), parts.end());
            const auto path = writeJpegBytes(ta.getPath("FILL_" + std::to_string(fill) + ".JPG"), body);
            EXPECT_EQ(fffDataOf(path), fill <= kMaxJpegFillBytes ? "ABCD" : "");
        }

        // 4 MB of RST markers, and of fill bytes: each probe reads a few KB of them, not the file
        const size_t size = static_cast<size_t>(4) << 20;
        std::vector<uint8_t> rstBody;
        for (size_t i = 0; i < size / 2; i++)
            rstBody.insert(rstBody.end(), {0xFF, 0xD0});
        rstBody.insert(rstBody.end(), parts.begin(), parts.end());
        std::vector<uint8_t> fillBody(size, 0xFF);
        fillBody.insert(fillBody.end(), parts.begin(), parts.end());
        const std::vector<std::pair<std::string, std::vector<uint8_t>>> files = {{"RST_4MB.JPG", rstBody},
                                                                                {"FILL_4MB.JPG", fillBody}};
        for (const auto &[name, body] : files)
        {
            SCOPED_TRACE(name);
            const auto path = writeJpegBytes(ta.getPath(name), body);
            ASSERT_GT(fs::file_size(path), size);
            const auto bytesOf = [&](const std::function<void()> &call) {
                const uint64_t before = thermalReadCountersForTesting().fileBytes;
                call();
                return thermalReadCountersForTesting().fileBytes - before;
            };
            const uint64_t bound = 2 * kMaxJpegMarkers + kMaxJpegFillBytes + 64;
            EXPECT_LE(bytesOf([&] { EXPECT_EQ(fffDataOf(path), ""); }), bound);
            EXPECT_LE(bytesOf([&] { EXPECT_FALSE(hasDjiRawThermal(path.string())); }), bound);
            EXPECT_LE(bytesOf([&] { EXPECT_FALSE(isThermalPicture("DJI", "FC220", path.string())); }), 2 * bound);
        }
    }

    // ---------------------------------------------------------------- DJI: what the APP3 segments can make the scan read

    // The DJI raw image of the bytes given (little-endian UInt16), in APP3 segments of chunk bytes with empty APP3
    // segments (FF E3 00 02) before each, then a frame header (SOF0) of the picture size given: what the probe reads
    std::vector<uint8_t> djiHeaders(const std::vector<uint8_t> &bytes, size_t chunk, int pictureWidth, int pictureHeight,
                                    size_t emptyBefore = 0)
    {
        std::vector<uint8_t> out = exifSegment("DJI", "ZZ-NEW-THERMAL-9");
        for (size_t at = 0; at < bytes.size(); at += chunk)
        {
            for (size_t i = 0; i < emptyBefore; i++)
                out.insert(out.end(), {0xFF, 0xE3, 0x00, 0x02});
            const size_t n = std::min(chunk, bytes.size() - at);
            const auto segment = segmentOf(0xE3, std::vector<uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                                                                      bytes.begin() + static_cast<std::ptrdiff_t>(at + n)));
            out.insert(out.end(), segment.begin(), segment.end());
        }
        const auto sof = segmentOf(0xC0, {0x08, static_cast<uint8_t>(pictureHeight >> 8),
                                          static_cast<uint8_t>(pictureHeight & 0xFF),
                                          static_cast<uint8_t>(pictureWidth >> 8),
                                          static_cast<uint8_t>(pictureWidth & 0xFF), 0x01, 0x01, 0x11, 0x00});
        out.insert(out.end(), sof.begin(), sof.end());
        return out;
    }

    std::vector<uint8_t> littleEndianBytes(const std::vector<uint16_t> &raw)
    {
        std::vector<uint8_t> bytes;
        for (const uint16_t v : raw)
            bytes.insert(bytes.end(), {static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>(v >> 8)});
        return bytes;
    }

    // An empty APP3 segment holds nothing of the raw image and is skipped (it was a 16-byte entry for 4 bytes of the
    // file); the raw image is read from at most kMaxDjiRawThermalChunks segments (256: the H30T has 41, the 640 x 512
    // cameras 11)
    TEST(RJpegThermal, theRawImageOfADjiPictureIsReadFromAtMost256NonEmptyApp3Segments)
    {
        TestArea ta(TEST_NAME);
        static_assert(kMaxDjiRawThermalChunks == 256, "the APP3 segments of a DJI raw image");
        const auto bytes = littleEndianBytes(rawGrid(kWidth, kHeight)); // 6144 bytes

        // 1000 empty APP3 segments before each of the 3 that hold the image
        const auto withEmpty = writeJpegBytes(ta.getPath("DJI_EMPTY.JPG"), djiHeaders(bytes, 2048, kWidth, kHeight, 1000));
        EXPECT_TRUE(hasDjiRawThermal(withEmpty.string()));
        const RawThermalData raw = extractRJpegRawThermal(withEmpty.string());
        ASSERT_TRUE(raw.valid);
        EXPECT_EQ(raw.width, kWidth);
        EXPECT_EQ(raw.height, kHeight);
        EXPECT_EQ(raw.data, rawGrid(kWidth, kHeight));

        // 256 segments of 24 bytes are read, 268 of 23 are not
        for (const size_t chunk : {static_cast<size_t>(24), static_cast<size_t>(23)})
        {
            SCOPED_TRACE(chunk);
            const size_t segments = (bytes.size() + chunk - 1) / chunk;
            EXPECT_EQ(segments <= kMaxDjiRawThermalChunks, chunk == 24);
            const auto path = writeJpegBytes(ta.getPath("DJI_CHUNK_" + std::to_string(chunk) + ".JPG"),
                                             djiHeaders(bytes, chunk, kWidth, kHeight));
            EXPECT_EQ(hasDjiRawThermal(path.string()), chunk == 24);
            const RawThermalData read = extractRJpegRawThermal(path.string());
            EXPECT_EQ(read.valid, chunk == 24);
            if (read.valid)
            {
                EXPECT_EQ(read.data, rawGrid(kWidth, kHeight));
            }
        }
    }

    // The APP3 segments of a DJI picture are read up to kMaxRawThermalPixels: the raw image of a 2048 x 2048 picture
    // (8 MB in 129 segments of 65532 bytes, as DJI writes them) is read, the one of 2048 x 2049 is not (it was read up to
    // 10000 x 10000, 200 MB, twice: the bytes of the segments and the counts)
    TEST(RJpegThermal, theRawImageOfADjiPictureIsReadUpToFourMegapixels)
    {
        TestArea ta(TEST_NAME);
        for (const int height : {2048, 2049})
        {
            SCOPED_TRACE(height);
            const size_t pixels = static_cast<size_t>(2048) * height;
            std::vector<uint16_t> raw(pixels);
            for (size_t i = 0; i < pixels; i++)
                raw[i] = static_cast<uint16_t>(i * 7);
            const auto path = writeJpegBytes(ta.getPath("DJI_" + std::to_string(height) + ".JPG"),
                                             djiHeaders(littleEndianBytes(raw), 65532, 2048, height));
            EXPECT_EQ(hasDjiRawThermal(path.string()), height == 2048);
            const RawThermalData read = extractRJpegRawThermal(path.string());
            EXPECT_EQ(read.valid, height == 2048);
            if (read.valid)
            {
                EXPECT_EQ(read.width, 2048);
                EXPECT_EQ(read.height, 2048);
                EXPECT_EQ(read.data, raw);
            }
        }
    }

    // ---------------------------------------------------------------- FLIR: the raw block of a segment without FFF data

    // A picture of a FLIR camera without FFF records nor XMP: the EXIF block, a FLIR APP1 segment ("FLIR\0", 3 bytes of
    // index, 16 bytes of zero) with the raw image of width x height counts (flirRawAt) when inSegment, the JPEG data,
    // then tailBytes after its end: the raw image, when it is not in the segment, then zeros
    fs::path writeFlirSegmentPicture(const fs::path &path, int width, int height, bool inSegment, size_t tailBytes)
    {
        const auto jpeg = plainJpeg(path.string() + ".tmp.jpg");
        std::vector<uint8_t> rawBytes;
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++)
            {
                const uint16_t v = flirRawAt(x, y);
                rawBytes.insert(rawBytes.end(), {static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>(v >> 8)});
            }
        std::vector<uint8_t> flir = {'F', 'L', 'I', 'R', 0, 0, 0, 0};
        flir.resize(flir.size() + 16, 0);
        if (inSegment)
            flir.insert(flir.end(), rawBytes.begin(), rawBytes.end());
        std::vector<uint8_t> tail;
        if (!inSegment)
            tail = rawBytes;
        tail.resize(std::max(tail.size(), tailBytes), 0);

        const auto out = join({{0xFF, 0xD8}, exifSegment("FLIR Systems AB", "FLIR Vue Pro R"), segmentOf(0xE1, flir),
                               std::vector<uint8_t>(jpeg.begin() + 2, jpeg.end()), tail});
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char *>(out.data()), static_cast<std::streamsize>(out.size()));
        return path;
    }

    // The raw block of a FLIR segment without FFF records is looked for in the segments from it to the scan, which alone
    // are read (the whole file was): a 160 x 120 block there is found by its size, with or without the tags (the raster
    // helper finds it by the common sizes), and one after the scan is not looked for
    TEST(RJpegThermal, theRawBlockOfAFlirSegmentWithoutFffDataIsLookedForInTheSegmentsBeforeTheScanAlone)
    {
        TestArea ta(TEST_NAME);
        const size_t tail = static_cast<size_t>(4) << 20;
        const auto bytesOf = [](const std::function<void()> &call) {
            const uint64_t before = thermalReadCountersForTesting().fileBytes;
            call();
            return thermalReadCountersForTesting().fileBytes - before;
        };

        const auto inSegment = writeFlirSegmentPicture(ta.getPath("FLIR_SEG.JPG"), 160, 120, true, tail);
        ASSERT_GT(fs::file_size(inSegment), tail);
        EXPECT_LT(bytesOf([&] { EXPECT_TRUE(isThermalImage(inSegment.string(), false)); }), static_cast<uint64_t>(200000));
        RawThermalData raw;
        EXPECT_LT(bytesOf([&] { raw = extractRJpegRawThermal(inSegment.string()); }), static_cast<uint64_t>(200000));
        ASSERT_TRUE(raw.valid);
        EXPECT_EQ(raw.width, 160);
        EXPECT_EQ(raw.height, 120);
        EXPECT_EQ(raw.data[0], flirRawAt(0, 0));
        EXPECT_EQ(raw.data[10 + 5 * 160], flirRawAt(10, 5));
        EXPECT_EQ(raw.data[159 + 119 * 160], flirRawAt(159, 119));

        const auto afterScan = writeFlirSegmentPicture(ta.getPath("FLIR_TAIL.JPG"), 160, 120, false, tail);
        EXPECT_LT(bytesOf([&] { EXPECT_FALSE(extractRJpegRawThermal(afterScan.string()).valid); }),
                  static_cast<uint64_t>(200000));
    }

    // The XMP size of the raw block of a FLIR segment without FFF records is that of a raw image: positive, and at most
    // kMaxRawThermalPixels; another one is not the file's, and the block is looked for at the common sizes. The block of
    // 2048 x 2048 counts is read at its XMP size; with an XMP size of 2048 x 2049 it is read as the 640 x 512 of a
    // common sensor, the first that fits; the 64 x 48 block of writeFlirPicture fits no common size
    TEST(RJpegThermal, theXmpSizeOfTheRawBlockOfAFlirSegmentIsPositiveAndAtMostFourMegapixels)
    {
        TestArea ta(TEST_NAME);
        struct Case
        {
            const char *width;
            const char *height;
            bool read;
        };
        int n = 0;
        for (const Case &c : {Case{"64", "48", true}, Case{"-64", "48", false}, Case{"64", "-48", false},
                              Case{"0", "48", false}, Case{"100000", "100000", false}, Case{"4294967360", "48", false}})
        {
            SCOPED_TRACE(std::string(c.width) + " x " + c.height);
            const auto path = writeFlirPicture(ta.getPath("FLIR_XMP_" + std::to_string(n++) + ".JPG"),
                                               {{"RawThermalImageWidth", c.width}, {"RawThermalImageHeight", c.height}});
            const RawThermalData raw = extractRJpegRawThermal(path.string());
            EXPECT_EQ(raw.valid, c.read);
            if (raw.valid)
            {
                EXPECT_EQ(raw.width, kWidth);
                EXPECT_EQ(raw.height, kHeight);
                EXPECT_EQ(raw.data[10 + 5 * kWidth], flirRawAt(10, 5));
            }
        }

        // A block of 8 MB in the FLIR APP1 segment and the 130 APP1 segments of 65532 bytes after it: counts of 0x4040,
        // whatever their alignment
        for (const int height : {2048, 2049})
        {
            SCOPED_TRACE(height);
            const auto jpeg = plainJpeg(ta.getPath("FLIR_BIG.tmp.jpg"));
            std::string xmp = std::string("http://ns.adobe.com/xap/1.0/") + '\0' +
                              "<?xpacket begin='' id='W5M0MpCehiHzreSzNTczkc9d'?>"
                              "<x:xmpmeta xmlns:x='adobe:ns:meta/'><rdf:RDF "
                              "xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>"
                              "<rdf:Description rdf:about='' xmlns:FLIR='http://ns.flir.com/xmp/1.0/' "
                              "FLIR:RawThermalImageWidth='2048' FLIR:RawThermalImageHeight='" +
                              std::to_string(height) + "'/></rdf:RDF></x:xmpmeta><?xpacket end='w'?>";
            std::vector<uint8_t> body = join({exifSegment("FLIR Systems AB", "FLIR Vue Pro R"), app1(xmp)});
            std::vector<uint8_t> first = {'F', 'L', 'I', 'R', 0};
            first.resize(65532, 0x40); // the index bytes too: the block starts after the magic
            const auto firstSegment = segmentOf(0xE1, first);
            body.insert(body.end(), firstSegment.begin(), firstSegment.end());
            const auto filler = segmentOf(0xE1, std::vector<uint8_t>(65532, 0x40));
            for (int i = 0; i < 130; i++)
                body.insert(body.end(), filler.begin(), filler.end());
            const auto out = join({{0xFF, 0xD8}, body, std::vector<uint8_t>(jpeg.begin() + 2, jpeg.end())});
            const auto path = ta.getPath("FLIR_BIG_" + std::to_string(height) + ".JPG");
            {
                std::ofstream file(path, std::ios::binary);
                file.write(reinterpret_cast<const char *>(out.data()), static_cast<std::streamsize>(out.size()));
            }

            const RawThermalData raw = extractRJpegRawThermal(path.string());
            ASSERT_TRUE(raw.valid);
            EXPECT_EQ(raw.width, height == 2048 ? 2048 : 640);
            EXPECT_EQ(raw.height, height == 2048 ? 2048 : 512);
            EXPECT_EQ(raw.data.size(), static_cast<size_t>(raw.width) * raw.height);
            EXPECT_EQ(raw.data.front(), 0x4040);
        }
    }

    // The probe of a raster that is no JPEG (a GeoTIFF, a COG of gigabytes, which the value info, the profile and the
    // vegetation analysis ask about) reads its first 2 bytes for the segments of an R-JPEG (the FLIR search read the whole
    // file, twice for a value info); its GDAL metadata still say it is thermal. In the raster helper, which opens it
    TEST(RJpegThermal, theThermalProbeOfARasterThatIsNoJpegReadsTwoBytesOfIt)
    {
        TestArea ta(TEST_NAME);
        const auto path = ta.getPath("kelvin.tif");
        {
            GDALDatasetH ds = GDALCreate(GDALGetDriverByName("GTiff"), path.string().c_str(), 1024, 1024, 1, GDT_Float32,
                                         nullptr);
            ASSERT_NE(ds, nullptr);
            GDALRasterBandH band = GDALGetRasterBand(ds, 1);
            GDALSetRasterUnitType(band, "Kelvin");
            std::vector<float> values(static_cast<size_t>(1024) * 1024, 290.0f);
            EXPECT_EQ(GDALRasterIO(band, GF_Write, 0, 0, 1024, 1024, values.data(), 1024, 1024, GDT_Float32, 0, 0),
                      CE_None);
            GDALClose(ds);
        }
        ASSERT_GT(fs::file_size(path), static_cast<uintmax_t>(4) << 20);

        testutils::runWithUserFileOpens([&] {
            const auto bytesOf = [](const std::function<void()> &call) {
                const uint64_t before = thermalReadCountersForTesting().fileBytes;
                call();
                return thermalReadCountersForTesting().fileBytes - before;
            };
            EXPECT_LE(bytesOf([&] { EXPECT_TRUE(isThermalImage(path.string(), false)); }), 2u);
            EXPECT_LE(bytesOf([&] { EXPECT_FALSE(extractRJpegRawThermal(path.string()).valid); }), 2u);
            EXPECT_LE(bytesOf([&] { EXPECT_FALSE(hasDjiRawThermal(path.string())); }), 2u);
        });
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

    TEST(RJpegThermal, theThumbnailOfADjiRJpegWithAColormapColoursItsRawCounts)
    {
        TestArea ta(TEST_NAME);
        const auto path = writeDjiPicture(ta.getPath("DJI_0001_R.JPG"), "DJI", "ZZ-NEW-THERMAL-9", rawGrid(kWidth, kHeight));

        // "rainbow" goes from (150, 0, 90) to (200, 0, 0). The hottest pixel (22240) has its last colour and the coldest
        // (15000) its first, whatever the grey of the preview is
        const auto image = thumbnailOf(path, nullptr, "rainbow", nullptr);
        ASSERT_EQ(image.width, kWidth);
        ASSERT_EQ(image.height, kHeight);
        expectColour(image, 63, 47, {200, 0, 0});
        expectColour(image, 0, 0, {150, 0, 90});
        EXPECT_NE(colourAt(image, 32, 24), (std::vector<int>{kPreviewGrey, kPreviewGrey, kPreviewGrey}));

        // A thermal formula gives raw counts no unit: the same picture, and its rescale (in degrees) does not apply
        const auto withFormula = thumbnailOf(path, "CELSIUS", "rainbow", "20,30");
        expectColour(withFormula, 63, 47, {200, 0, 0});
        expectColour(withFormula, 0, 0, {150, 0, 90});
    }

    TEST(RJpegThermal, theRescaleOfTheThumbnailOfADjiRJpegIsInRawCounts)
    {
        TestArea ta(TEST_NAME);
        const auto path = writeDjiPicture(ta.getPath("DJI_0001_R.JPG"), "DJI", "ZZ-NEW-THERMAL-9", rawGrid(kWidth, kHeight));

        // 15000 is the coldest pixel, 16100 the pixel (10, 5): a range of 17000 to 19000 puts both under the first
        // colour, the hottest (22240) over the last, and the pixel (32, 24), 18680, at (18680 - 17000) / 2000 of the map
        const auto image = thumbnailOf(path, nullptr, "grayscale", "17000,19000");
        expectColour(image, 0, 0, {0, 0, 0});
        expectColour(image, 10, 5, {0, 0, 0});
        expectColour(image, 63, 47, {255, 255, 255});
        EXPECT_NEAR(colourAt(image, 32, 24)[0], 0.84 * 255, 8.0);
    }
} // namespace
