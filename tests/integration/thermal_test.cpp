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
#include <cstdlib>
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

// A raw count the calibration cannot convert has no temperature (NaN, a pixel without a value), not 0 degrees Celsius:
// with O = -7340, the counts up to 7340 make R2 * (raw + O) zero or negative
TEST(thermal, aRawCountOutsideTheCalibrationHasNoTemperature) {
    ThermalCalibration cal;
    cal.planckR1 = 21106.77;
    cal.planckB = 1501.0;
    cal.planckF = 1.0;
    cal.planckO = -7340.0;
    cal.planckR2 = 0.012545258;
    cal.valid = true;

    EXPECT_TRUE(std::isnan(rawToTemperature(0, cal)));
    EXPECT_TRUE(std::isnan(rawToTemperature(7340, cal)));
    EXPECT_NEAR(rawToTemperature(15104, cal), 5.685, 0.001);

    RawThermalData raw;
    raw.data = {0, 15104};
    raw.width = 2;
    raw.height = 1;
    raw.valid = true;
    const std::vector<float> map = rawToTemperatureMap(raw, cal);
    ASSERT_EQ(map.size(), 2u);
    EXPECT_TRUE(std::isnan(map[0]));
    EXPECT_NEAR(map[1], 5.685, 0.001);
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

// A DJI R-JPEG has no calibration: its raw counts are not those of the Planck formula (they need DJI's own
// conversion), and no measurement parameter is reported that the file does not say (this one has no DJI XMP)
TEST(thermal, extractCalibration) {
    TestArea ta(TEST_NAME);
    fs::path thermalImg = ta.downloadTestAsset(
        THERMAL_BASE_URL + "DJI_0001_R.JPG", "DJI_0001_R.JPG");

    auto cal = extractThermalCalibration(thermalImg.string());
    EXPECT_FALSE(cal.valid);
    EXPECT_FALSE(cal.emissivity.has_value());
    EXPECT_FALSE(cal.objectDistance.has_value());
    EXPECT_FALSE(cal.reflectedApparentTemperature.has_value());
    EXPECT_FALSE(cal.relativeHumidity.has_value());
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

// The raw counts of a DJI R-JPEG are read, and no temperature is told from them: DJI's conversion is not available
// (djiRawToTemperatureMap, the extension point of dji_irp), and the Planck formula of FLIR is not it
TEST(thermal, rawToTempMap) {
    TestArea ta(TEST_NAME);
    fs::path thermalImg = ta.downloadTestAsset(
        THERMAL_BASE_URL + "DJI_0001_R.JPG", "DJI_0001_R.JPG");

    const auto rjpeg = readRJpegThermal(thermalImg.string());
    ASSERT_TRUE(rjpeg.has_value());
    EXPECT_TRUE(rjpeg->raw.valid);
    EXPECT_FALSE(rjpeg->hasTemperatures());
    EXPECT_FALSE(rjpeg->calibration.valid);
    EXPECT_FALSE(djiRawToTemperatureMap(rjpeg->raw, thermalImg.string()).has_value());
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

    // A DJI R-JPEG: its raw counts, no calibration, no unit
    EXPECT_FALSE(j.contains("calibration"));
    EXPECT_EQ(j["unit"], "");
    EXPECT_FALSE(j["isDirectValue"].get<bool>());
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

    // The raw count of the pixel, without a unit (a DJI R-JPEG has no temperatures)
    EXPECT_EQ(j["value"], j["rawValue"]);
    EXPECT_EQ(j["unit"], "");
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
    EXPECT_FALSE(j.contains("calibration"));
    EXPECT_EQ(j["unit"], "");
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

// The pixel exports with options on a DJI R-JPEG (FR-RAS-14, FR-RAS-31, D-022): its raw counts have no unit, so a
// thermal formula is refused (no calibration), and the raw value of the pixel as NoData leaves it without a value
TEST(thermal, cApiPixelExportsTakeTheOptions) {
    TestArea ta(TEST_NAME);
    fs::path thermalImg = ta.downloadTestAsset(
        THERMAL_BASE_URL + "DJI_0001_R.JPG", "DJI_0001_R.JPG");
    const std::string path = thermalImg.string();

    char *output = nullptr;
    ASSERT_EQ(DDBGetRasterPointValuePixelEx(path.c_str(), 50, 50, nullptr, &output), DDBERR_NONE) << DDBGetLastError();
    const json plain = json::parse(std::string(output));
    DDBFree(output);
    EXPECT_EQ(plain["unit"], "");

    DDBRasterOptions options = {};
    options.structSize = sizeof(options);
    options.formula = "KELVIN";
    output = nullptr;
    EXPECT_EQ(DDBGetRasterPointValuePixelEx(path.c_str(), 50, 50, &options, &output), DDBERR_INVALIDARGS);
    EXPECT_NE(std::string(DDBGetLastError()).find("No thermal calibration"), std::string::npos) << DDBGetLastError();
    output = nullptr;
    EXPECT_EQ(DDBGetRasterAreaStatsPixelEx(path.c_str(), 10, 10, 100, 100, &options, &output), DDBERR_INVALIDARGS);

    ASSERT_EQ(DDBGetRasterAreaStats(path.c_str(), 10, 10, 100, 100, &output), DDBERR_NONE);
    const json areaPlain = json::parse(std::string(output));
    DDBFree(output);
    EXPECT_EQ(areaPlain["unit"], "");

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

// The DJI_0001_R.JPG of the SDK: M2EA (Mavic 2 Enterprise Advanced), H20T and H20N, 640 x 512. The H20T is
// test_data/thermal/DJI_0001_R.JPG and must be there: its tests fail when it cannot be had. M2EA and H20N are to be added to
// test_data as thermal/M2EA/DJI_0001_R.JPG and thermal/H20N/DJI_0001_R.JPG: until then their tests are skipped, and fail
// when DDB_REQUIRE_DJI_SDK_SAMPLES is set (a CI that has them). The raw counts of a DJI R-JPEG need DJI's own
// conversion, which DroneDB does not have: the raster API reports them as raw counts without a unit. The expected raw
// counts are read here by GDAL from the DJI_RAW_THERMAL_IMAGE subdataset of the JPEG driver, not by the code under test;
// the literals are those of gdalinfo -stats of that subdataset.
struct DjiSample {
    const char *camera;
    const char *model;
    const char *asset;      // path under test_data
    bool inTestData;        // in the test_data commit of tests/test-data.sha
    int rawMin, rawMax;     // raw counts, the extremes of the image (gdalinfo -stats)
    int rawAtCentre;        // the pixel (320, 256)
    int previewAtCentre;    // the byte of the colour preview there, which the raster API used to report as degrees
};

const DjiSample DJI_SAMPLES[] = {
    {"M2EA", "MAVIC2-ENTERPRISE-ADVANCED", "thermal/M2EA/DJI_0001_R.JPG", false, 17228, 20036, 18384, 31},
    {"H20T", "ZH20T", "thermal/DJI_0001_R.JPG", true, 13537, 20670, 15908, 147},
    {"H20N", "ZH20N", "thermal/H20N/DJI_0001_R.JPG", false, 17836, 19568, 18764, 140},
};

// The name of a sample in the messages of gtest
void PrintTo(const DjiSample &sample, std::ostream *os) {
    *os << sample.camera;
}

// The sample copied in the area. One in test_data must be had (a failure otherwise, as for any asset). One that is not
// there yet is read from the download cache when it is there (DDB_TEST_CACHE_DIR), is downloaded when
// DDB_REQUIRE_DJI_SDK_SAMPLES is set (a failure if it cannot be), and is a skip otherwise (no download attempt that is
// known to fail)
#define DJI_SAMPLE_OR_SKIP(name, ta, sample)                                                                        \
    fs::path name;                                                                                                  \
    {                                                                                                               \
        const std::string url = testDataUrl((sample).asset);                                                        \
        const bool required = (sample).inTestData || std::getenv("DDB_REQUIRE_DJI_SDK_SAMPLES") != nullptr;          \
        if (!required && !fs::exists(TestFS::cachedAssetPath(url)))                                                 \
            GTEST_SKIP() << (sample).asset << " is not in test_data yet (DJI thermal SDK sample; set "              \
                         << "DDB_REQUIRE_DJI_SDK_SAMPLES to make this a failure)";                                  \
        try {                                                                                                       \
            name = (ta).downloadTestAsset(url, std::string((sample).camera) + "_DJI_0001_R.JPG");                   \
        } catch (const std::exception &e) {                                                                         \
            FAIL() << (sample).asset << " (DJI thermal SDK sample) could not be had: " << e.what();                 \
        }                                                                                                           \
    }

// The raw thermal image of a DJI R-JPEG as GDAL reads it (the DJI_RAW_THERMAL_IMAGE subdataset of its JPEG driver):
// the counts, row by row, and the extremes, the pixel of the hottest count
struct GdalRawThermal {
    int width = 0, height = 0;
    std::vector<uint16_t> counts;
    int min = 0, max = 0;
    int hotX = 0, hotY = 0;
};

GdalRawThermal gdalRawThermal(const fs::path &img) {
    GdalRawThermal r;
    const std::string name = "JPEG:\"" + img.string() + "\":DJI_RAW_THERMAL_IMAGE";
    GDALDatasetH ds = GDALOpen(name.c_str(), GA_ReadOnly);
    EXPECT_NE(ds, nullptr) << name;
    if (ds == nullptr) return r;
    r.width = GDALGetRasterXSize(ds);
    r.height = GDALGetRasterYSize(ds);
    r.counts.resize(static_cast<size_t>(r.width) * r.height);
    EXPECT_EQ(GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Read, 0, 0, r.width, r.height, r.counts.data(), r.width,
                           r.height, GDT_UInt16, 0, 0),
              CE_None);
    GDALClose(ds);
    const auto [lo, hi] = std::minmax_element(r.counts.begin(), r.counts.end());
    r.min = *lo;
    r.max = *hi;
    const size_t hot = static_cast<size_t>(hi - r.counts.begin());
    r.hotX = static_cast<int>(hot % r.width);
    r.hotY = static_cast<int>(hot / r.width);
    return r;
}

// One test per sample, so that a sample that is not in test_data yet skips only its own
class DjiThermalSdkSample : public ::testing::TestWithParam<DjiSample> {};

INSTANTIATE_TEST_SUITE_P(thermal, DjiThermalSdkSample, ::testing::ValuesIn(DJI_SAMPLES),
                         [](const ::testing::TestParamInfo<DjiSample> &sample) { return std::string(sample.param.camera); });

TEST_P(DjiThermalSdkSample, isThermalWithRawCountsAndNoCalibration) {
    const DjiSample &sample = GetParam();
    TestArea ta(TEST_NAME);
    DJI_SAMPLE_OR_SKIP(img, ta, sample);
    const GdalRawThermal gdal = gdalRawThermal(img);
    ASSERT_EQ(gdal.width, 640);
    ASSERT_EQ(gdal.height, 512);
    EXPECT_EQ(gdal.min, sample.rawMin);
    EXPECT_EQ(gdal.max, sample.rawMax);

    EXPECT_TRUE(isThermalImageFromExif("DJI", sample.model));
    EXPECT_TRUE(hasDjiRawThermal(img.string()));
    EXPECT_TRUE(isThermalImage(img.string()));
    // No calibration: the Planck formula of FLIR is not DJI's conversion
    EXPECT_FALSE(extractThermalCalibration(img.string()).valid);

    const auto raw = extractRawThermalData(img.string());
    ASSERT_TRUE(raw.valid);
    EXPECT_EQ(raw.width, 640);
    EXPECT_EQ(raw.height, 512);
    EXPECT_TRUE(raw.data == gdal.counts) << "the raw counts are those GDAL reads";
    EXPECT_EQ(raw.data[320 + 256 * 640], sample.rawAtCentre);

    const auto rjpeg = readRJpegThermal(img.string());
    ASSERT_TRUE(rjpeg.has_value());
    EXPECT_FALSE(rjpeg->hasTemperatures());
    EXPECT_FALSE(rjpeg->calibration.valid);

    Entry entry;
    parseEntry(img, ta.getFolder(), entry, false);
    EXPECT_EQ(entry.properties.value("sensorCategory", ""), "thermal");
}

TEST_P(DjiThermalSdkSample, theRasterApiReportsRawCountsWithoutAUnit) {
    const DjiSample &sample = GetParam();
    TestArea ta(TEST_NAME);
    DJI_SAMPLE_OR_SKIP(img, ta, sample);
    const std::string path = img.string();
    const GdalRawThermal gdal = gdalRawThermal(img);
    ASSERT_EQ(gdal.counts.size(), static_cast<size_t>(640) * 512);

    char *output = nullptr;
    ASSERT_EQ(DDBGetRasterValueInfo(path.c_str(), &output), DDBERR_NONE) << DDBGetLastError();
    const auto info = json::parse(std::string(output));
    DDBFree(output);
    EXPECT_EQ(info["width"], 640);
    EXPECT_EQ(info["height"], 512);
    EXPECT_TRUE(info["isThermal"].get<bool>());
    EXPECT_FALSE(info["isDirectValue"].get<bool>());
    EXPECT_EQ(info["unit"], "");
    EXPECT_FALSE(info.contains("calibration"));
    EXPECT_EQ(info["valueMin"].get<int>(), gdal.min);
    EXPECT_EQ(info["valueMax"].get<int>(), gdal.max);
    ASSERT_EQ(info["stretch"].size(), 1u);
    EXPECT_EQ(info["stretch"][0]["min"].get<int>(), gdal.min);
    EXPECT_EQ(info["stretch"][0]["max"].get<int>(), gdal.max);

    // The pixel (320, 256) is the raw count of the grid, without a unit: not a temperature, not the byte of the
    // colour preview (147 for the H20T)
    ASSERT_EQ(DDBGetRasterPointValuePixelEx(path.c_str(), 320, 256, nullptr, &output), DDBERR_NONE) << DDBGetLastError();
    const auto point = json::parse(std::string(output));
    DDBFree(output);
    EXPECT_EQ(point["value"].get<int>(), gdal.counts[320 + 256 * 640]);
    EXPECT_EQ(point["rawValue"].get<int>(), sample.rawAtCentre);
    EXPECT_NE(point["value"].get<int>(), sample.previewAtCentre);
    EXPECT_EQ(point["unit"], "");

    // The pixel with the hottest raw count is the maximum of the value info
    ASSERT_EQ(DDBGetRasterPointValuePixelEx(path.c_str(), gdal.hotX, gdal.hotY, nullptr, &output), DDBERR_NONE);
    EXPECT_EQ(json::parse(std::string(output))["value"].get<int>(), gdal.max);
    DDBFree(output);

    // No temperature can be asked for
    DDBRasterOptions celsius = {};
    celsius.structSize = sizeof(celsius);
    celsius.formula = "CELSIUS";
    output = nullptr;
    EXPECT_EQ(DDBGetRasterPointValuePixelEx(path.c_str(), 320, 256, &celsius, &output), DDBERR_INVALIDARGS);
}

// The thumbnail of a DJI R-JPEG with a colormap colours its raw counts from their minimum to their maximum: the hottest
// pixel has the last colour of the map ("rainbow" ends at 200, 0, 0), the picture has the size of the raw thermal image,
// with a thermal formula (which gives raw counts no unit) or none
TEST_P(DjiThermalSdkSample, theThumbnailColoursTheRawCounts) {
    const DjiSample &sample = GetParam();
    TestArea ta(TEST_NAME);
    DJI_SAMPLE_OR_SKIP(img, ta, sample);
    const GdalRawThermal gdal = gdalRawThermal(img);
    ASSERT_EQ(gdal.counts.size(), static_cast<size_t>(640) * 512);

    for (const char *formula : {"CELSIUS", static_cast<const char *>(nullptr)}) {
        SCOPED_TRACE(formula != nullptr ? formula : "no formula");
        uint8_t *buffer = nullptr;
        int size = 0;
        ASSERT_EQ(DDBGenerateMemoryThumbnailEx(img.string().c_str(), 640, nullptr, nullptr, formula, nullptr,
                                               "rainbow", nullptr, &buffer, &size),
                  DDBERR_NONE) << DDBGetLastError();
        const auto image = testutils::decodeImage(std::vector<uint8_t>(buffer, buffer + size));
        DDBVSIFree(buffer);
        ASSERT_EQ(image.width, 640);
        ASSERT_EQ(image.height, 512);

        const size_t hot = static_cast<size_t>(gdal.hotY) * 640 + gdal.hotX;
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
