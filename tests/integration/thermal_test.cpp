/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "json.h"
#include "test.h"
#include "testarea.h"
#include "testfs.h"

#include "thermal.h"
#include "raster_analysis.h"
#include "ddb.h"
#include "sensorprofile.h"
#include "gdal_inc.h"
#include "entry.h"
#include "raster_test_utils.h"

#include "mio.h"

#include <algorithm>
#include <cmath>
#include <fstream>

namespace {

using namespace ddb;

const std::string THERMAL_BASE_URL = testDataUrl("thermal/");

// ============================================================
// Detection Tests
// ============================================================

TEST(thermal, detectThermalFromExif) {
    EXPECT_TRUE(isThermalImageFromExif("DJI", "MAVIC3T-T"));
    EXPECT_TRUE(isThermalImageFromExif("DJI", "ZH20T"));
    EXPECT_TRUE(isThermalImageFromExif("DJI", "ZH20T-S"));
    EXPECT_TRUE(isThermalImageFromExif("DJI", "M3T"));
    EXPECT_TRUE(isThermalImageFromExif("FLIR", "Vue Pro R"));
    EXPECT_TRUE(isThermalImageFromExif("FLIR", "Tau2"));
    EXPECT_TRUE(isThermalImageFromExif("FLIR Systems AB", "FLIR A310"));
    EXPECT_TRUE(isThermalImageFromExif("Workswell", "WirisProSc"));

    EXPECT_FALSE(isThermalImageFromExif("DJI", "FC220"));
    EXPECT_FALSE(isThermalImageFromExif("Hasselblad", "L1D-20c"));
    EXPECT_FALSE(isThermalImageFromExif("Canon", "EOS R5"));
}

// ============================================================
// Planck Formula Tests
// ============================================================

TEST(thermal, planckFormula) {
    ThermalCalibration cal;
    cal.planckR1 = 21106.77;
    cal.planckB = 1501.0;
    cal.planckF = 1.0;
    cal.planckO = -7340.0;
    cal.planckR2 = 0.012545258;
    cal.emissivity = 0.95;
    cal.valid = true;

    // A typical midrange raw value should give a reasonable temperature
    uint16_t rawMid = 20000;
    double temp = rawToTemperature(rawMid, cal);
    // Expected: a reasonable temperature in Celsius (roughly room temp to warm)
    EXPECT_GT(temp, -50.0);
    EXPECT_LT(temp, 200.0);

    // Higher raw values should give higher temperatures
    uint16_t rawHigh = 30000;
    double tempHigh = rawToTemperature(rawHigh, cal);
    EXPECT_GT(tempHigh, temp);

    // Lower raw values should give lower temperatures
    uint16_t rawLow = 10000;
    double tempLow = rawToTemperature(rawLow, cal);
    EXPECT_LT(tempLow, temp);
}

// ============================================================
// R-JPEG Detection & Parsing (requires test data download)
// ============================================================

TEST(thermal, detectThermalImageFile) {
    TestArea ta(TEST_NAME);
    fs::path thermalImg = ta.downloadTestAsset(
        THERMAL_BASE_URL + "DJI_0001_R.JPG", "DJI_0001_R.JPG");

    EXPECT_TRUE(isThermalImage(thermalImg.string()));
}

TEST(thermal, extractCalibration) {
    TestArea ta(TEST_NAME);
    fs::path thermalImg = ta.downloadTestAsset(
        THERMAL_BASE_URL + "DJI_0001_R.JPG", "DJI_0001_R.JPG");

    auto cal = extractThermalCalibration(thermalImg.string());
    EXPECT_TRUE(cal.valid);
    EXPECT_GT(cal.planckR1, 0.0);
    EXPECT_GT(cal.planckB, 0.0);
    EXPECT_GT(cal.emissivity, 0.0);
    EXPECT_LE(cal.emissivity, 1.0);
}

TEST(thermal, extractRawData) {
    TestArea ta(TEST_NAME);
    fs::path thermalImg = ta.downloadTestAsset(
        THERMAL_BASE_URL + "DJI_0001_R.JPG", "DJI_0001_R.JPG");

    auto raw = extractRawThermalData(thermalImg.string());
    EXPECT_TRUE(raw.valid);
    EXPECT_GT(raw.width, 0);
    EXPECT_GT(raw.height, 0);
    EXPECT_EQ(raw.data.size(), static_cast<size_t>(raw.width * raw.height));
}

TEST(thermal, rawToTempMap) {
    TestArea ta(TEST_NAME);
    fs::path thermalImg = ta.downloadTestAsset(
        THERMAL_BASE_URL + "DJI_0001_R.JPG", "DJI_0001_R.JPG");

    auto raw = extractRawThermalData(thermalImg.string());
    auto cal = extractThermalCalibration(thermalImg.string());

    ASSERT_TRUE(raw.valid);
    ASSERT_TRUE(cal.valid);

    auto tempMap = rawToTemperatureMap(raw, cal);
    EXPECT_EQ(tempMap.size(), raw.data.size());

    // All temperatures should be in a plausible range
    for (auto t : tempMap) {
        EXPECT_GT(t, -80.0f);
        EXPECT_LT(t, 600.0f);
    }
}

// ============================================================
// JSON Query Functions
// ============================================================

TEST(thermal, rasterValueInfoJson) {
    TestArea ta(TEST_NAME);
    fs::path thermalImg = ta.downloadTestAsset(
        THERMAL_BASE_URL + "DJI_0001_R.JPG", "DJI_0001_R.JPG");

    std::string jsonStr = getRasterValueInfoJson(thermalImg.string());
    auto j = json::parse(jsonStr);

    EXPECT_TRUE(j.contains("calibration"));
    EXPECT_TRUE(j.contains("valueMin"));
    EXPECT_TRUE(j.contains("valueMax"));
    EXPECT_TRUE(j.contains("width"));
    EXPECT_TRUE(j.contains("height"));
    EXPECT_TRUE(j.contains("isThermal"));
    EXPECT_TRUE(j["isThermal"].get<bool>());
    EXPECT_GT(j["width"].get<int>(), 0);
    EXPECT_GT(j["height"].get<int>(), 0);
    EXPECT_LT(j["valueMin"].get<double>(), j["valueMax"].get<double>());
}

TEST(thermal, rasterPointJson) {
    TestArea ta(TEST_NAME);
    fs::path thermalImg = ta.downloadTestAsset(
        THERMAL_BASE_URL + "DJI_0001_R.JPG", "DJI_0001_R.JPG");

    std::string jsonStr = getRasterPointJson(thermalImg.string(), 100, 100);
    auto j = json::parse(jsonStr);

    EXPECT_TRUE(j.contains("value"));
    EXPECT_TRUE(j.contains("rawValue"));
    EXPECT_TRUE(j.contains("x"));
    EXPECT_TRUE(j.contains("y"));
    EXPECT_TRUE(j.contains("isThermal"));
    EXPECT_EQ(j["x"].get<int>(), 100);
    EXPECT_EQ(j["y"].get<int>(), 100);

    double val = j["value"].get<double>();
    EXPECT_GT(val, -80.0);
    EXPECT_LT(val, 600.0);
}

TEST(thermal, rasterAreaStatsJson) {
    TestArea ta(TEST_NAME);
    fs::path thermalImg = ta.downloadTestAsset(
        THERMAL_BASE_URL + "DJI_0001_R.JPG", "DJI_0001_R.JPG");

    std::string jsonStr = getRasterAreaStatsJson(thermalImg.string(), 50, 50, 150, 150);
    auto j = json::parse(jsonStr);

    EXPECT_TRUE(j.contains("min"));
    EXPECT_TRUE(j.contains("max"));
    EXPECT_TRUE(j.contains("mean"));
    EXPECT_TRUE(j.contains("stddev"));
    EXPECT_TRUE(j.contains("median"));
    EXPECT_TRUE(j.contains("pixelCount"));
    EXPECT_TRUE(j.contains("isThermal"));

    EXPECT_GT(j["pixelCount"].get<int>(), 0);
    EXPECT_LE(j["min"].get<double>(), j["max"].get<double>());
    EXPECT_GE(j["mean"].get<double>(), j["min"].get<double>());
    EXPECT_LE(j["mean"].get<double>(), j["max"].get<double>());
}

// ============================================================
// C API Tests
// ============================================================

TEST(thermal, cApiGetRasterValueInfo) {
    TestArea ta(TEST_NAME);
    fs::path thermalImg = ta.downloadTestAsset(
        THERMAL_BASE_URL + "DJI_0001_R.JPG", "DJI_0001_R.JPG");

    char *output = nullptr;
    auto err = DDBGetRasterValueInfo(thermalImg.string().c_str(), &output);
    EXPECT_EQ(err, DDBERR_NONE);
    EXPECT_NE(output, nullptr);

    auto j = json::parse(std::string(output));
    EXPECT_TRUE(j.contains("calibration"));
    EXPECT_TRUE(j.contains("valueMin"));

    DDBFree(output);
}

TEST(thermal, cApiGetRasterPointValue) {
    TestArea ta(TEST_NAME);
    fs::path thermalImg = ta.downloadTestAsset(
        THERMAL_BASE_URL + "DJI_0001_R.JPG", "DJI_0001_R.JPG");

    char *output = nullptr;
    auto err = DDBGetRasterPointValue(thermalImg.string().c_str(), 50, 50, &output);
    EXPECT_EQ(err, DDBERR_NONE);
    EXPECT_NE(output, nullptr);

    auto j = json::parse(std::string(output));
    EXPECT_TRUE(j.contains("value"));

    DDBFree(output);
}

TEST(thermal, cApiGetRasterAreaStats) {
    TestArea ta(TEST_NAME);
    fs::path thermalImg = ta.downloadTestAsset(
        THERMAL_BASE_URL + "DJI_0001_R.JPG", "DJI_0001_R.JPG");

    char *output = nullptr;
    auto err = DDBGetRasterAreaStats(thermalImg.string().c_str(), 10, 10, 100, 100, &output);
    EXPECT_EQ(err, DDBERR_NONE);
    EXPECT_NE(output, nullptr);

    auto j = json::parse(std::string(output));
    EXPECT_TRUE(j.contains("min"));
    EXPECT_GT(j["pixelCount"].get<int>(), 0);

    DDBFree(output);
}

// The pixel exports with options on an R-JPEG (FR-RAS-15, FR-RAS-31, D-022): the value of a pixel in K is the
// value in degrees Celsius plus 273.15, and the raw value of the pixel as NoData leaves it without a value
TEST(thermal, cApiPixelExportsTakeTheOptions) {
    TestArea ta(TEST_NAME);
    fs::path thermalImg = ta.downloadTestAsset(
        THERMAL_BASE_URL + "DJI_0001_R.JPG", "DJI_0001_R.JPG");
    const std::string path = thermalImg.string();

    char *output = nullptr;
    ASSERT_EQ(DDBGetRasterPointValuePixelEx(path.c_str(), 50, 50, nullptr, &output), DDBERR_NONE) << DDBGetLastError();
    const json plain = json::parse(std::string(output));
    DDBFree(output);
    EXPECT_EQ(plain["unit"], "\xC2\xB0" "C");

    DDBRasterOptions options = {};
    options.structSize = sizeof(options);
    options.formula = "KELVIN";
    ASSERT_EQ(DDBGetRasterPointValuePixelEx(path.c_str(), 50, 50, &options, &output), DDBERR_NONE) << DDBGetLastError();
    const json kelvin = json::parse(std::string(output));
    DDBFree(output);
    EXPECT_EQ(kelvin["unit"], "K");
    EXPECT_NEAR(kelvin["value"].get<double>(), plain["value"].get<double>() + 273.15, 1e-3);

    ASSERT_EQ(DDBGetRasterAreaStatsPixelEx(path.c_str(), 10, 10, 100, 100, &options, &output), DDBERR_NONE)
        << DDBGetLastError();
    const json area = json::parse(std::string(output));
    DDBFree(output);
    ASSERT_EQ(DDBGetRasterAreaStats(path.c_str(), 10, 10, 100, 100, &output), DDBERR_NONE);
    const json areaPlain = json::parse(std::string(output));
    DDBFree(output);
    EXPECT_EQ(area["unit"], "K");
    EXPECT_EQ(area["pixelCount"], areaPlain["pixelCount"]);
    EXPECT_NEAR(area["mean"].get<double>(), areaPlain["mean"].get<double>() + 273.15, 1e-3);

    // The raw value of the pixel as NoData: the pixel has no value, and the area leaves out every pixel with it
    options.formula = nullptr;
    options.flags = DDB_RASTER_OPT_NODATA;
    options.nodata = plain["rawValue"].get<double>();
    EXPECT_EQ(DDBGetRasterPointValuePixelEx(path.c_str(), 50, 50, &options, &output), DDBERR_INVALIDARGS);
    output = nullptr;
    const DDBErr err = DDBGetRasterAreaStatsPixelEx(path.c_str(), 10, 10, 100, 100, &options, &output);
    if (err == DDBERR_NONE) {
        EXPECT_LT(json::parse(std::string(output))["pixelCount"].get<int>(), areaPlain["pixelCount"].get<int>());
        DDBFree(output);
    } else {
        EXPECT_EQ(err, DDBERR_INVALIDARGS); // every pixel of the area had that value
    }
}

// ============================================================
// The DJI thermal SDK samples (U-603: FR-RAS-13, FR-RAS-14, FR-RAS-15)
// ============================================================

// The DJI_0001_R.JPG of the SDK: M2EA (Mavic 2 Enterprise Advanced), H20T and H20N, 640 x 512. M2EA and H20N are
// in test_data/thermal/<camera>/, the H20T is test_data/thermal/DJI_0001_R.JPG. The raw counts and the pixels are
// those of the DJI_RAW_THERMAL_IMAGE subdataset (gdalinfo -stats, a read of the array) and the temperatures are
// those of the Planck formula with the default calibration, computed outside the code under test.
struct DjiSample {
    const char *camera;
    const char *model;
    const char *asset;      // path under test_data
    int rawMin, rawMax;     // raw counts, the extremes of the image
    double tempMin, tempMax;
    int rawAtCentre;        // the pixel (320, 256)
    double tempAtCentre;
    int previewAtCentre;    // the byte of the colour preview there, which the raster API used to report as degrees
    int hotX, hotY;         // the one pixel with the hottest raw count
};

const DjiSample DJI_SAMPLES[] = {
    {"M2EA", "MAVIC2-ENTERPRISE-ADVANCED", "thermal/M2EA/DJI_0001_R.JPG", 17228, 20036, 18.7289, 33.5372, 18384, 25.1017, 31, 637, 450},
    {"H20T", "ZH20T", "thermal/DJI_0001_R.JPG", 13537, 20670, -5.4784, 36.5976, 15908, 10.8586, 147, 260, 353},
    {"H20N", "ZH20N", "thermal/H20N/DJI_0001_R.JPG", 17836, 19568, 22.1346, 31.2187, 18764, 27.1066, 140, 470, 326},
};

// The sample copied in the area, or a skipped test when it is not in test_data (M2EA and H20N come with the test_data
// that follows this change)
fs::path djiSample(TestArea &ta, const DjiSample &sample) {
    try {
        return ta.downloadTestAsset(testDataUrl(sample.asset), std::string(sample.camera) + "_DJI_0001_R.JPG");
    } catch (const std::exception &e) {
        return {};
    }
}

#define DJI_SAMPLE_OR_SKIP(name, ta, sample)                                                            \
    const fs::path name = djiSample(ta, sample);                                                          \
    if (name.empty()) GTEST_SKIP() << sample.asset << " is not in test_data (DJI thermal SDK sample)"

TEST(thermal, theDjiSdkSamplesAreThermalWithCalibration) {
    for (const auto &sample : DJI_SAMPLES) {
        SCOPED_TRACE(sample.camera);
        TestArea ta(TEST_NAME);
        DJI_SAMPLE_OR_SKIP(img, ta, sample);

        EXPECT_TRUE(isThermalImageFromExif("DJI", sample.model));
        EXPECT_TRUE(hasDjiRawThermal(img.string()));
        EXPECT_TRUE(isThermalImage(img.string()));
        EXPECT_TRUE(extractThermalCalibration(img.string()).valid);

        const auto raw = extractRawThermalData(img.string());
        ASSERT_TRUE(raw.valid);
        EXPECT_EQ(raw.width, 640);
        EXPECT_EQ(raw.height, 512);
        EXPECT_EQ(*std::min_element(raw.data.begin(), raw.data.end()), sample.rawMin);
        EXPECT_EQ(*std::max_element(raw.data.begin(), raw.data.end()), sample.rawMax);
        EXPECT_EQ(raw.data[320 + 256 * 640], sample.rawAtCentre);

        Entry entry;
        parseEntry(img, ta.getFolder(), entry, false);
        EXPECT_EQ(entry.properties.value("sensorCategory", ""), "thermal");
    }
}

TEST(thermal, theRasterApiOfTheDjiSdkSamplesReportsTemperatures) {
    for (const auto &sample : DJI_SAMPLES) {
        SCOPED_TRACE(sample.camera);
        TestArea ta(TEST_NAME);
        DJI_SAMPLE_OR_SKIP(img, ta, sample);
        const std::string path = img.string();

        char *output = nullptr;
        ASSERT_EQ(DDBGetRasterValueInfo(path.c_str(), &output), DDBERR_NONE) << DDBGetLastError();
        const auto info = json::parse(std::string(output));
        DDBFree(output);
        EXPECT_EQ(info["width"], 640);
        EXPECT_EQ(info["height"], 512);
        EXPECT_TRUE(info["isThermal"].get<bool>());
        EXPECT_FALSE(info["isDirectValue"].get<bool>());
        EXPECT_EQ(info["unit"], "\xC2\xB0" "C");
        EXPECT_TRUE(info.contains("calibration"));
        EXPECT_NEAR(info["valueMin"].get<double>(), sample.tempMin, 0.01);
        EXPECT_NEAR(info["valueMax"].get<double>(), sample.tempMax, 0.01);

        // The pixel (320, 256) is a temperature, not the byte of the colour preview (147 for the H20T)
        ASSERT_EQ(DDBGetRasterPointValuePixelEx(path.c_str(), 320, 256, nullptr, &output), DDBERR_NONE) << DDBGetLastError();
        const auto point = json::parse(std::string(output));
        DDBFree(output);
        EXPECT_NEAR(point["value"].get<double>(), sample.tempAtCentre, 0.01);
        EXPECT_NE(static_cast<int>(point["value"].get<double>()), sample.previewAtCentre);
        EXPECT_EQ(point["rawValue"], sample.rawAtCentre);
        EXPECT_EQ(point["unit"], "\xC2\xB0" "C");

        // The pixel with the hottest raw count is the maximum of the value info
        ASSERT_EQ(DDBGetRasterPointValuePixelEx(path.c_str(), sample.hotX, sample.hotY, nullptr, &output), DDBERR_NONE);
        EXPECT_NEAR(json::parse(std::string(output))["value"].get<double>(), sample.tempMax, 0.01);
        DDBFree(output);
    }
}

// The thumbnail of an R-JPEG with a colormap colours its raw temperatures: the hottest pixel has the last colour of the
// map ("rainbow" ends at 200, 0, 0 and starts at 150, 0, 90), the picture has the size of the raw thermal image
TEST(thermal, theThumbnailOfTheDjiSdkSamplesColoursTheRawTemperatures) {
    for (const auto &sample : DJI_SAMPLES) {
        SCOPED_TRACE(sample.camera);
        TestArea ta(TEST_NAME);
        DJI_SAMPLE_OR_SKIP(img, ta, sample);

        uint8_t *buffer = nullptr;
        int size = 0;
        ASSERT_EQ(DDBGenerateMemoryThumbnailEx(img.string().c_str(), 640, nullptr, nullptr, "CELSIUS", nullptr, "rainbow",
                                               nullptr, &buffer, &size),
                  DDBERR_NONE) << DDBGetLastError();
        const auto image = testutils::decodeImage(std::vector<uint8_t>(buffer, buffer + size));
        DDBVSIFree(buffer);
        ASSERT_EQ(image.width, 640);
        ASSERT_EQ(image.height, 512);

        const size_t hot = static_cast<size_t>(sample.hotY) * 640 + sample.hotX;
        EXPECT_NEAR(image.bands[0][hot], 200, 16);
        EXPECT_NEAR(image.bands[1][hot], 0, 16);
        EXPECT_NEAR(image.bands[2][hot], 0, 16);
    }
}

// ============================================================
// Non-thermal file should not be detected
// ============================================================

TEST(thermal, nonThermalDetection) {
    TestArea ta(TEST_NAME);
    // Create a regular 3-band Byte GeoTIFF
    GDALDriverH tifDrv = GDALGetDriverByName("GTiff");
    ASSERT_NE(tifDrv, nullptr);

    fs::path rasterPath = ta.getPath("regular_rgb.tif");
    GDALDatasetH hDs = GDALCreate(tifDrv, rasterPath.string().c_str(), 100, 100, 3, GDT_Byte, nullptr);
    ASSERT_NE(hDs, nullptr);
    GDALClose(hDs);

    EXPECT_FALSE(isThermalImage(rasterPath.string()));
}

// ============================================================
// Keyword detection on GDAL metadata
// ============================================================

// A single-band Float32 GeoTIFF with no pixel data written; the band description, the unit and
// one dataset metadata item are set only when given
fs::path writeSingleBandTiff(const fs::path &path, const std::string &bandDescription = "",
                             const std::string &unit = "", const std::string &noteItem = "") {
    fs::create_directories(path.parent_path());
    GDALDriverH tifDrv = GDALGetDriverByName("GTiff");
    EXPECT_NE(tifDrv, nullptr);
    GDALDatasetH hDs = GDALCreate(tifDrv, path.string().c_str(), 16, 16, 1, GDT_Float32, nullptr);
    EXPECT_NE(hDs, nullptr);
    GDALRasterBandH hBand = GDALGetRasterBand(hDs, 1);
    if (!bandDescription.empty()) GDALSetDescription(hBand, bandDescription.c_str());
    if (!unit.empty()) GDALSetRasterUnitType(hBand, unit.c_str());
    if (!noteItem.empty()) GDALSetMetadataItem(hDs, "NOTE", noteItem.c_str(), nullptr);
    GDALClose(hDs);
    return path;
}

TEST(thermal, folderNameWithShortKeywordIsNotThermal) {
    TestArea ta(TEST_NAME);
    // GDAL lists the path of the file in its DERIVED_SUBDATASETS domain: a folder called
    // "subtitles_bt" must not make a plain raster thermal
    const fs::path rasterPath = writeSingleBandTiff(ta.getPath("subtitles_bt/plain.tif"));

    EXPECT_FALSE(isThermalImage(rasterPath.string()));
}

TEST(thermal, bandDescriptionLstIsThermal) {
    TestArea ta(TEST_NAME);
    const fs::path rasterPath = writeSingleBandTiff(ta.getPath("lst.tif"), "LST");

    EXPECT_TRUE(isThermalImage(rasterPath.string()));
}

TEST(thermal, bandUnitKelvinIsThermal) {
    TestArea ta(TEST_NAME);
    const fs::path rasterPath = writeSingleBandTiff(ta.getPath("kelvin.tif"), "", "Kelvin");

    EXPECT_TRUE(isThermalImage(rasterPath.string()));
}

TEST(thermal, wordContainingDemDoesNotRuleOutThermal) {
    TestArea ta(TEST_NAME);
    const fs::path rasterPath =
        writeSingleBandTiff(ta.getPath("academic.tif"), "", "Kelvin", "academic survey");

    EXPECT_TRUE(isThermalImage(rasterPath.string()));
}

TEST(thermal, demTokenRulesOutThermal) {
    TestArea ta(TEST_NAME);
    const fs::path rasterPath = writeSingleBandTiff(ta.getPath("dem.tif"), "", "Kelvin", "DEM of the site");

    EXPECT_FALSE(isThermalImage(rasterPath.string()));
}

// ============================================================
// Sensor Profile Detection for Thermal
// ============================================================

TEST(thermal, sensorProfilesIncludeThermal) {
    // The sensor profiles file shipped next to the library includes thermal profiles
    // that match when a thermal raster is provided
    std::ifstream file(io::fromUtf8(io::toUtf8(io::getDataPath("sensor-profiles.json"))));
    ASSERT_TRUE(file.is_open());

    bool hasThermal = false;
    for (const auto& p : json::parse(file).at("profiles").get<std::vector<SensorProfile>>()) {
        if (p.sensorCategory == "thermal") {
            hasThermal = true;
            break;
        }
    }
    EXPECT_TRUE(hasThermal);
}

} // namespace
