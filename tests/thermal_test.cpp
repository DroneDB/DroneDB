/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"

#include "thermal.h"
#include "ddb.h"
#include "sensorprofile.h"
#include "gdal_inc.h"

#include <cmath>

namespace {

using namespace ddb;

const std::string THERMAL_BASE_URL = "https://github.com/DroneDB/test_data/raw/refs/heads/master/thermal/";

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

TEST(thermal, thermalInfoJson) {
    TestArea ta(TEST_NAME);
    fs::path thermalImg = ta.downloadTestAsset(
        THERMAL_BASE_URL + "DJI_0001_R.JPG", "DJI_0001_R.JPG");

    std::string jsonStr = getThermalInfoJson(thermalImg.string());
    auto j = json::parse(jsonStr);

    EXPECT_TRUE(j.contains("calibration"));
    EXPECT_TRUE(j.contains("temperatureMin"));
    EXPECT_TRUE(j.contains("temperatureMax"));
    EXPECT_TRUE(j.contains("width"));
    EXPECT_TRUE(j.contains("height"));
    EXPECT_GT(j["width"].get<int>(), 0);
    EXPECT_GT(j["height"].get<int>(), 0);
    EXPECT_LT(j["temperatureMin"].get<double>(), j["temperatureMax"].get<double>());
}

TEST(thermal, thermalPointJson) {
    TestArea ta(TEST_NAME);
    fs::path thermalImg = ta.downloadTestAsset(
        THERMAL_BASE_URL + "DJI_0001_R.JPG", "DJI_0001_R.JPG");

    std::string jsonStr = getThermalPointJson(thermalImg.string(), 100, 100);
    auto j = json::parse(jsonStr);

    EXPECT_TRUE(j.contains("temperature"));
    EXPECT_TRUE(j.contains("rawValue"));
    EXPECT_TRUE(j.contains("x"));
    EXPECT_TRUE(j.contains("y"));
    EXPECT_EQ(j["x"].get<int>(), 100);
    EXPECT_EQ(j["y"].get<int>(), 100);

    double temp = j["temperature"].get<double>();
    EXPECT_GT(temp, -80.0);
    EXPECT_LT(temp, 600.0);
}

TEST(thermal, thermalAreaStatsJson) {
    TestArea ta(TEST_NAME);
    fs::path thermalImg = ta.downloadTestAsset(
        THERMAL_BASE_URL + "DJI_0001_R.JPG", "DJI_0001_R.JPG");

    std::string jsonStr = getThermalAreaStatsJson(thermalImg.string(), 50, 50, 150, 150);
    auto j = json::parse(jsonStr);

    EXPECT_TRUE(j.contains("min"));
    EXPECT_TRUE(j.contains("max"));
    EXPECT_TRUE(j.contains("mean"));
    EXPECT_TRUE(j.contains("stddev"));
    EXPECT_TRUE(j.contains("median"));
    EXPECT_TRUE(j.contains("pixelCount"));

    EXPECT_GT(j["pixelCount"].get<int>(), 0);
    EXPECT_LE(j["min"].get<double>(), j["max"].get<double>());
    EXPECT_GE(j["mean"].get<double>(), j["min"].get<double>());
    EXPECT_LE(j["mean"].get<double>(), j["max"].get<double>());
}

// ============================================================
// C API Tests
// ============================================================

TEST(thermal, cApiGetThermalInfo) {
    TestArea ta(TEST_NAME);
    fs::path thermalImg = ta.downloadTestAsset(
        THERMAL_BASE_URL + "DJI_0001_R.JPG", "DJI_0001_R.JPG");

    char *output = nullptr;
    auto err = DDBGetThermalInfo(thermalImg.string().c_str(), &output);
    EXPECT_EQ(err, DDBERR_NONE);
    EXPECT_NE(output, nullptr);

    auto j = json::parse(std::string(output));
    EXPECT_TRUE(j.contains("calibration"));
    EXPECT_TRUE(j.contains("temperatureMin"));

    DDBFree(output);
}

TEST(thermal, cApiGetThermalPoint) {
    TestArea ta(TEST_NAME);
    fs::path thermalImg = ta.downloadTestAsset(
        THERMAL_BASE_URL + "DJI_0001_R.JPG", "DJI_0001_R.JPG");

    char *output = nullptr;
    auto err = DDBGetThermalPoint(thermalImg.string().c_str(), 50, 50, &output);
    EXPECT_EQ(err, DDBERR_NONE);
    EXPECT_NE(output, nullptr);

    auto j = json::parse(std::string(output));
    EXPECT_TRUE(j.contains("temperature"));

    DDBFree(output);
}

TEST(thermal, cApiGetThermalAreaStats) {
    TestArea ta(TEST_NAME);
    fs::path thermalImg = ta.downloadTestAsset(
        THERMAL_BASE_URL + "DJI_0001_R.JPG", "DJI_0001_R.JPG");

    char *output = nullptr;
    auto err = DDBGetThermalAreaStats(thermalImg.string().c_str(), 10, 10, 100, 100, &output);
    EXPECT_EQ(err, DDBERR_NONE);
    EXPECT_NE(output, nullptr);

    auto j = json::parse(std::string(output));
    EXPECT_TRUE(j.contains("min"));
    EXPECT_GT(j["pixelCount"].get<int>(), 0);

    DDBFree(output);
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
// Sensor Profile Detection for Thermal
// ============================================================

TEST(thermal, sensorProfileDetectsFlir) {
    auto& spm = SensorProfileManager::instance();
    spm.loadDefaults();

    // The sensor profiles JSON should include thermal profiles
    // that match when a thermal raster is provided
    auto profiles = spm.getProfiles();
    bool hasThermal = false;
    for (const auto& p : profiles) {
        if (p.sensorCategory == "thermal") {
            hasThermal = true;
            break;
        }
    }
    EXPECT_TRUE(hasThermal);
}

} // namespace
