/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Radiometric R-JPEGs (U-603: FR-RAS-13, FR-RAS-14, FR-RAS-15, FR-XC-24, D-012, D-022). A DJI R-JPEG carries the raw
// thermal image of the camera in its APP3 segments; a FLIR R-JPEG carries it in its FLIR APP1 segment, and its Planck
// calibration in its XMP. These tests write such pictures (a JPEG, an EXIF block with the make and model, the raw image)
// so that no download is needed; the DJI thermal SDK samples (M2EA, H20T, H20N) are in
// tests/integration/thermal_test.cpp. The raw counts of a DJI camera need DJI's own conversion, which DroneDB does not
// have: the raster API gives them as raw counts without a unit. The expected temperatures of the FLIR picture are
// literals computed outside the code under test: the Planck formula (R1 21106.77, B 1501, F 1, O -7340, R2 0.012545258)
// of the raw counts 15104 -> 5.6850 C, 15124 -> 5.8177 C, 15204 -> 6.3464 C, 15224 -> 6.4780 C, 16204 -> 12.6860 C,
// 18784 -> 27.2110 C, 22344 -> 44.2823 C.

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

        // No temperature can be asked for: there is no calibration (DJI's conversion is not available)
        const auto [err, message] = pointErrorOf(path, 0, 0, "KELVIN");
        EXPECT_EQ(err, DDBERR_INVALIDARGS);
        EXPECT_NE(message.find("No thermal calibration"), std::string::npos) << message;
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

        // A thermal formula cannot make degrees of the colours (the byte 147 was 147 degrees Celsius)
        const auto [err, message] = pointErrorOf(path, 10, 5, "CELSIUS");
        EXPECT_EQ(err, DDBERR_INVALIDARGS);
        EXPECT_NE(message.find("No thermal calibration"), std::string::npos) << message;
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
