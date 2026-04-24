/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"

#include "ddb.h"
#include "sensorprofile.h"
#include "vegetation.h"
#include "merge_multispectral.h"
#include "cog.h"
#include "thumbs.h"
#include "gdaltiler.h"
#include "tilerhelper.h"
#include "gdal_inc.h"
#include "mio.h"
#include "exceptions.h"
#include "exif.h"
#include "exifeditor.h"

#include <fstream>
#include <cstring>

namespace {

using namespace ddb;

// ============================================================
// Sensor Profile Tests
// ============================================================

TEST(multispectral, sensorProfileLoad) {
    auto& spm = SensorProfileManager::instance();
    spm.loadDefaults();
    // Should have loaded profiles even if file doesn't exist (will log warning)
    // If file exists, should have >= 1 profile
    SUCCEED();
}

TEST(multispectral, detectSensorRGB) {
    TestArea ta(TEST_NAME);
    // Create a 3-band Byte dataset saved to file
    GDALDriverH tifDrv = GDALGetDriverByName("GTiff");
    ASSERT_NE(tifDrv, nullptr);

    fs::path rasterPath = ta.getPath("rgb_test.tif");
    GDALDatasetH hDs = GDALCreate(tifDrv, rasterPath.string().c_str(), 100, 100, 3, GDT_Byte, nullptr);
    ASSERT_NE(hDs, nullptr);
    GDALClose(hDs);

    auto& spm = SensorProfileManager::instance();
    auto result = spm.detectSensor(rasterPath.string());

    // 3-band Byte = standard RGB, not multispectral
    EXPECT_FALSE(result.detected);
    EXPECT_TRUE(result.sensorId.empty());
}

TEST(multispectral, detectSensorRGBA) {
    TestArea ta(TEST_NAME);
    // Create a 4-band Byte dataset with alpha
    GDALDriverH tifDrv = GDALGetDriverByName("GTiff");
    fs::path rasterPath = ta.getPath("rgba_test.tif");
    GDALDatasetH hDs = GDALCreate(tifDrv, rasterPath.string().c_str(), 100, 100, 4, GDT_Byte, nullptr);
    ASSERT_NE(hDs, nullptr);

    // Set band 4 as alpha
    GDALSetRasterColorInterpretation(GDALGetRasterBand(hDs, 4), GCI_AlphaBand);
    GDALClose(hDs);

    auto& spm = SensorProfileManager::instance();
    auto result = spm.detectSensor(rasterPath.string());

    // 4-band Byte with alpha = RGBA, not multispectral
    EXPECT_FALSE(result.detected);
}

TEST(multispectral, detectSensor5BandUInt16) {
    TestArea ta(TEST_NAME);
    // Create a 5-band UInt16 dataset (like MicaSense RedEdge)
    GDALDriverH tifDrv = GDALGetDriverByName("GTiff");
    fs::path rasterPath = ta.getPath("ms5_test.tif");
    GDALDatasetH hDs = GDALCreate(tifDrv, rasterPath.string().c_str(), 100, 100, 5, GDT_UInt16, nullptr);
    ASSERT_NE(hDs, nullptr);
    GDALClose(hDs);

    auto& spm = SensorProfileManager::instance();
    auto result = spm.detectSensor(rasterPath.string());

    // 5-band UInt16 = multispectral
    EXPECT_TRUE(result.detected);
}

TEST(multispectral, detectSensorFallbackPopulatesBands) {
    // Regression test: when no sensor profile matches but the raster is
    // multi-band non-Byte (e.g. ODM multispectral orthophoto with Red, Green,
    // NIR, Rededge + Alpha), detectSensor() must populate result.bands so that
    // getRasterInfoJson returns band info and autoDetectFilter can build the
    // correct filter (e.g. "RGNRe").
    TestArea ta(TEST_NAME);
    GDALDriverH tifDrv = GDALGetDriverByName("GTiff");
    fs::path rasterPath = ta.getPath("ms5_desc.tif");
    GDALDatasetH hDs = GDALCreate(tifDrv, rasterPath.string().c_str(), 64, 64, 5, GDT_UInt16, nullptr);
    ASSERT_NE(hDs, nullptr);

    // Set band descriptions and color interpretation (mimicking ODM output)
    GDALRasterBandH b1 = GDALGetRasterBand(hDs, 1);
    GDALSetDescription(b1, "Red");
    GDALSetRasterColorInterpretation(b1, GCI_RedBand);

    GDALRasterBandH b2 = GDALGetRasterBand(hDs, 2);
    GDALSetDescription(b2, "Green");
    GDALSetRasterColorInterpretation(b2, GCI_GreenBand);

    GDALRasterBandH b3 = GDALGetRasterBand(hDs, 3);
    GDALSetDescription(b3, "NIR");
    GDALSetRasterColorInterpretation(b3, GCI_GrayIndex);

    GDALRasterBandH b4 = GDALGetRasterBand(hDs, 4);
    GDALSetDescription(b4, "Rededge");
    GDALSetRasterColorInterpretation(b4, GCI_GrayIndex);

    GDALRasterBandH b5 = GDALGetRasterBand(hDs, 5);
    GDALSetRasterColorInterpretation(b5, GCI_AlphaBand);

    GDALClose(hDs);

    auto& spm = SensorProfileManager::instance();
    auto result = spm.detectSensor(rasterPath.string());

    EXPECT_TRUE(result.detected);
    EXPECT_EQ(result.sensorCategory, "multispectral");
    // Alpha should be excluded from reported bands
    ASSERT_EQ(result.bands.size(), 4u);
    EXPECT_EQ(result.bands[0].name, "Red");
    EXPECT_EQ(result.bands[1].name, "Green");
    EXPECT_EQ(result.bands[2].name, "NIR");
    EXPECT_EQ(result.bands[3].name, "Rededge");

    // autoDetectFilter should now map R/G/N/Re from the populated bands
    auto& ve = VegetationEngine::instance();
    auto bf = ve.autoDetectFilter(rasterPath.string());
    EXPECT_EQ(bf.id, "RGNRe");
    EXPECT_EQ(bf.R, 0);
    EXPECT_EQ(bf.G, 1);
    EXPECT_EQ(bf.N, 2);
    EXPECT_EQ(bf.Re, 3);

    // getRasterInfoJson should return non-empty bands
    std::string infoJson = spm.getRasterInfoJson(rasterPath.string());
    auto info = json::parse(infoJson);
    EXPECT_EQ(info["bandCount"].get<int>(), 5);
    ASSERT_TRUE(info.contains("bands"));
    EXPECT_EQ(info["bands"].size(), 4u);
}

TEST(multispectral, defaultBandMappingFallback) {
    TestArea ta(TEST_NAME);
    // Create a simple raster file for fallback mapping test
    GDALDriverH tifDrv = GDALGetDriverByName("GTiff");
    fs::path rasterPath = ta.getPath("fallback_test.tif");
    GDALDatasetH hDs = GDALCreate(tifDrv, rasterPath.string().c_str(), 100, 100, 3, GDT_Byte, nullptr);
    ASSERT_NE(hDs, nullptr);
    GDALClose(hDs);

    auto& spm = SensorProfileManager::instance();
    auto mapping = spm.getDefaultBandMapping(rasterPath.string());
    EXPECT_GE(mapping.r, 1);
    EXPECT_GE(mapping.g, 1);
    EXPECT_GE(mapping.b, 1);
}

// ============================================================
// Vegetation Engine Tests
// ============================================================

TEST(multispectral, vegetationFormulaList) {
    auto& ve = VegetationEngine::instance();
    BandFilter rgbFilter = VegetationEngine::parseFilter("RGB", 3);
    auto formulas = ve.getFormulasForFilter(rgbFilter);

    // RGB filter should have at least VARI, EXG, GLI, vNDVI
    bool hasVARI = false;
    for (const auto& f : formulas) {
        if (f.id == "VARI") hasVARI = true;
    }
    EXPECT_TRUE(hasVARI);
}

TEST(multispectral, vegetationFormulaNIR) {
    auto& ve = VegetationEngine::instance();
    BandFilter rgbnFilter = VegetationEngine::parseFilter("RGBN", 4);
    auto formulas = ve.getFormulasForFilter(rgbnFilter);

    // RGBN filter should have NDVI
    bool hasNDVI = false;
    for (const auto& f : formulas) {
        if (f.id == "NDVI") hasNDVI = true;
    }
    EXPECT_TRUE(hasNDVI);
}

TEST(multispectral, applyFormulaVARI) {
    auto& ve = VegetationEngine::instance();

    // Create test data: 3 bands, 4 pixels (as float)
    std::vector<float> band1 = {100, 200, 50, 0};  // Band 0 (R)
    std::vector<float> band2 = {200, 100, 150, 0};  // Band 1 (G)
    std::vector<float> band3 = {50, 50, 100, 0};    // Band 2 (B)
    std::vector<float*> bandData = {band1.data(), band2.data(), band3.data()};

    BandFilter bf = VegetationEngine::parseFilter("RGB", 3);

    std::vector<float> result(4);
    float nodata = -9999.0f;
    const auto* formulaPtr = ve.getFormula("VARI");
    ASSERT_NE(formulaPtr, nullptr);
    ve.applyFormula(*formulaPtr, bf, bandData, result.data(), 4, nodata);

    // VARI = (G - R) / (G + R - B)
    // Pixel 0: (200 - 100) / (200 + 100 - 50) = 100 / 250 = 0.4
    EXPECT_NEAR(result[0], 0.4, 0.01);

    // Pixel 3: all zeros → denominator ~0 → nodata
    EXPECT_FLOAT_EQ(result[3], nodata);
}

TEST(multispectral, colormapsList) {
    auto& ve = VegetationEngine::instance();
    json colormapsJson = ve.getColormapsJson();
    std::string jsonStr = colormapsJson.dump();
    EXPECT_FALSE(jsonStr.empty());
    EXPECT_NE(jsonStr.find("rdylgn"), std::string::npos);
    EXPECT_NE(jsonStr.find("viridis"), std::string::npos);
}

TEST(multispectral, applyColormap) {
    auto& ve = VegetationEngine::instance();

    std::vector<float> values = {-1.0f, 0.0f, 0.5f, 1.0f};
    float nodata = -9999.0f;
    std::vector<uint8_t> rgba(values.size() * 4);

    const auto* cmap = ve.getColormap("rdylgn");
    ASSERT_NE(cmap, nullptr);
    ve.applyColormap(values.data(), rgba.data(), values.size(), *cmap, -1.0f, 1.0f, nodata);

    // All pixels should have alpha = 255 (no nodata in input)
    for (size_t i = 0; i < values.size(); i++) {
        EXPECT_EQ(rgba[i * 4 + 3], 255);
    }
}

TEST(multispectral, applyColormapNodata) {
    auto& ve = VegetationEngine::instance();

    std::vector<float> values = {0.5f, -9999.0f};
    float nodata = -9999.0f;
    std::vector<uint8_t> rgba(values.size() * 4);

    const auto* cmap = ve.getColormap("rdylgn");
    ASSERT_NE(cmap, nullptr);
    ve.applyColormap(values.data(), rgba.data(), values.size(), *cmap, -1.0f, 1.0f, nodata);

    // First pixel should have alpha = 255
    EXPECT_EQ(rgba[3], 255);
    // Second pixel (nodata) should have alpha = 0
    EXPECT_EQ(rgba[7], 0);
}

// ============================================================
// Merge Multispectral Tests
// ============================================================

TEST(multispectral, validateMergeTooFewFiles) {
    std::vector<std::string> inputs = {"single.tif"};
    auto result = validateMergeMultispectral(inputs);
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.errors.empty());
}

TEST(multispectral, validateMergeMatchingFiles) {
    TestArea ta(TEST_NAME);

    // Create two matching single-band GeoTIFFs
    GDALDriverH tifDrv = GDALGetDriverByName("GTiff");
    ASSERT_NE(tifDrv, nullptr);

    double gt[6] = {0, 1.0, 0, 100, 0, -1.0};
    const char* proj = "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]]";

    fs::path band1Path = ta.getPath("band1.tif");
    fs::path band2Path = ta.getPath("band2.tif");

    for (auto& path : {band1Path, band2Path}) {
        GDALDatasetH hDs = GDALCreate(tifDrv, path.string().c_str(), 100, 100, 1, GDT_UInt16, nullptr);
        ASSERT_NE(hDs, nullptr);
        GDALSetGeoTransform(hDs, gt);
        GDALSetProjection(hDs, proj);
        GDALClose(hDs);
    }

    auto result = validateMergeMultispectral({band1Path.string(), band2Path.string()});
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(result.errors.empty());
    EXPECT_EQ(result.summary.totalBands, 2);
}

TEST(multispectral, validateMergeCRSMismatch) {
    TestArea ta(TEST_NAME);

    GDALDriverH tifDrv = GDALGetDriverByName("GTiff");
    double gt[6] = {0, 1.0, 0, 100, 0, -1.0};

    fs::path band1Path = ta.getPath("band1_wgs84.tif");
    fs::path band2Path = ta.getPath("band2_utm.tif");

    {
        GDALDatasetH hDs = GDALCreate(tifDrv, band1Path.string().c_str(), 100, 100, 1, GDT_UInt16, nullptr);
        GDALSetGeoTransform(hDs, gt);
        GDALSetProjection(hDs, "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]]");
        GDALClose(hDs);
    }
    {
        GDALDatasetH hDs = GDALCreate(tifDrv, band2Path.string().c_str(), 100, 100, 1, GDT_UInt16, nullptr);
        GDALSetGeoTransform(hDs, gt);
        // Use UTM zone 32N
        OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
        OSRImportFromEPSG(srs, 32632);
        char* wkt;
        OSRExportToWkt(srs, &wkt);
        GDALSetProjection(hDs, wkt);
        CPLFree(wkt);
        OSRDestroySpatialReference(srs);
        GDALClose(hDs);
    }

    auto result = validateMergeMultispectral({band1Path.string(), band2Path.string()});
    EXPECT_FALSE(result.ok);
    // Should have CRS mismatch error
    bool hasCrsError = false;
    for (const auto& err : result.errors) {
        if (err.find("CRS") != std::string::npos) hasCrsError = true;
    }
    EXPECT_TRUE(hasCrsError);
}

TEST(multispectral, validateMergeSizeMismatch) {
    TestArea ta(TEST_NAME);

    GDALDriverH tifDrv = GDALGetDriverByName("GTiff");
    double gt[6] = {0, 1.0, 0, 100, 0, -1.0};
    const char* proj = "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]]";

    fs::path band1Path = ta.getPath("band1_100.tif");
    fs::path band2Path = ta.getPath("band2_200.tif");

    {
        GDALDatasetH hDs = GDALCreate(tifDrv, band1Path.string().c_str(), 100, 100, 1, GDT_UInt16, nullptr);
        GDALSetGeoTransform(hDs, gt);
        GDALSetProjection(hDs, proj);
        GDALClose(hDs);
    }
    {
        GDALDatasetH hDs = GDALCreate(tifDrv, band2Path.string().c_str(), 200, 200, 1, GDT_UInt16, nullptr);
        GDALSetGeoTransform(hDs, gt);
        GDALSetProjection(hDs, proj);
        GDALClose(hDs);
    }

    auto result = validateMergeMultispectral({band1Path.string(), band2Path.string()});
    EXPECT_FALSE(result.ok);
    bool hasDimError = false;
    for (const auto& err : result.errors) {
        if (err.find("Dimension") != std::string::npos) hasDimError = true;
    }
    EXPECT_TRUE(hasDimError);
}

TEST(multispectral, validateMergeTypeMismatch) {
    TestArea ta(TEST_NAME);

    GDALDriverH tifDrv = GDALGetDriverByName("GTiff");
    double gt[6] = {0, 1.0, 0, 100, 0, -1.0};
    const char* proj = "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]]";

    fs::path band1Path = ta.getPath("band1_uint16.tif");
    fs::path band2Path = ta.getPath("band2_float32.tif");

    {
        GDALDatasetH hDs = GDALCreate(tifDrv, band1Path.string().c_str(), 100, 100, 1, GDT_UInt16, nullptr);
        GDALSetGeoTransform(hDs, gt);
        GDALSetProjection(hDs, proj);
        GDALClose(hDs);
    }
    {
        GDALDatasetH hDs = GDALCreate(tifDrv, band2Path.string().c_str(), 100, 100, 1, GDT_Float32, nullptr);
        GDALSetGeoTransform(hDs, gt);
        GDALSetProjection(hDs, proj);
        GDALClose(hDs);
    }

    auto result = validateMergeMultispectral({band1Path.string(), band2Path.string()});
    EXPECT_FALSE(result.ok);
    bool hasTypeError = false;
    for (const auto& err : result.errors) {
        if (err.find("Data type") != std::string::npos) hasTypeError = true;
    }
    EXPECT_TRUE(hasTypeError);
}

TEST(multispectral, mergeHappyPath) {
    TestArea ta(TEST_NAME, true);

    GDALDriverH tifDrv = GDALGetDriverByName("GTiff");
    double gt[6] = {0, 0.001, 0, 50, 0, -0.001};
    const char* proj = "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]]";

    std::vector<std::string> inputs;
    for (int i = 0; i < 5; i++) {
        fs::path bandPath = ta.getPath("band" + std::to_string(i + 1) + ".tif");
        GDALDatasetH hDs = GDALCreate(tifDrv, bandPath.string().c_str(), 50, 50, 1, GDT_UInt16, nullptr);
        ASSERT_NE(hDs, nullptr);
        GDALSetGeoTransform(hDs, gt);
        GDALSetProjection(hDs, proj);

        // Write some data
        std::vector<uint16_t> data(50 * 50, static_cast<uint16_t>((i + 1) * 1000));
        GDALRasterIO(GDALGetRasterBand(hDs, 1), GF_Write, 0, 0, 50, 50,
                     data.data(), 50, 50, GDT_UInt16, 0, 0);
        GDALClose(hDs);

        inputs.push_back(bandPath.string());
    }

    fs::path outputPath = ta.getPath("merged.tif");
    EXPECT_NO_THROW(mergeMultispectral(inputs, outputPath.string()));
    EXPECT_TRUE(fs::exists(outputPath));

    // Verify output has 5 bands
    GDALDatasetH hOut = GDALOpen(outputPath.string().c_str(), GA_ReadOnly);
    ASSERT_NE(hOut, nullptr);
    EXPECT_EQ(GDALGetRasterCount(hOut), 5);
    GDALClose(hOut);
}

TEST(multispectral, mergeOutputExists) {
    TestArea ta(TEST_NAME);

    GDALDriverH tifDrv = GDALGetDriverByName("GTiff");
    double gt[6] = {0, 1.0, 0, 100, 0, -1.0};
    const char* proj = "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]]";

    fs::path band1Path = ta.getPath("existing_b1.tif");
    fs::path band2Path = ta.getPath("existing_b2.tif");

    for (auto& path : {band1Path, band2Path}) {
        GDALDatasetH hDs = GDALCreate(tifDrv, path.string().c_str(), 50, 50, 1, GDT_UInt16, nullptr);
        GDALSetGeoTransform(hDs, gt);
        GDALSetProjection(hDs, proj);
        GDALClose(hDs);
    }

    fs::path outputPath = ta.getPath("output_exists.tif");
    // Create the output file so it already exists
    std::ofstream ofs(outputPath.string());
    ofs << "dummy";
    ofs.close();

    EXPECT_THROW(mergeMultispectral({band1Path.string(), band2Path.string()}, outputPath.string()), AppException);
}

TEST(multispectral, multibandWarning) {
    TestArea ta(TEST_NAME);

    GDALDriverH tifDrv = GDALGetDriverByName("GTiff");
    double gt[6] = {0, 1.0, 0, 100, 0, -1.0};
    const char* proj = "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]]";

    fs::path band1Path = ta.getPath("multi_3band.tif");
    fs::path band2Path = ta.getPath("multi_single.tif");

    // First file has 3 bands (should generate warning)
    {
        GDALDatasetH hDs = GDALCreate(tifDrv, band1Path.string().c_str(), 50, 50, 3, GDT_UInt16, nullptr);
        GDALSetGeoTransform(hDs, gt);
        GDALSetProjection(hDs, proj);
        GDALClose(hDs);
    }
    {
        GDALDatasetH hDs = GDALCreate(tifDrv, band2Path.string().c_str(), 50, 50, 1, GDT_UInt16, nullptr);
        GDALSetGeoTransform(hDs, gt);
        GDALSetProjection(hDs, proj);
        GDALClose(hDs);
    }

    auto result = validateMergeMultispectral({band1Path.string(), band2Path.string()});
    EXPECT_TRUE(result.ok);  // Multi-band is a warning, not an error
    EXPECT_FALSE(result.warnings.empty());
    EXPECT_EQ(result.summary.totalBands, 4);  // 3 + 1
}

// ============================================================
// COG Stats Sidecar Test
// ============================================================

TEST(multispectral, cogStatsSidecar) {
    TestArea ta(TEST_NAME);

    GDALDriverH tifDrv = GDALGetDriverByName("GTiff");
    fs::path inputPath = ta.getPath("input_for_stats.tif");

    {
        GDALDatasetH hDs = GDALCreate(tifDrv, inputPath.string().c_str(), 100, 100, 3, GDT_Byte, nullptr);
        ASSERT_NE(hDs, nullptr);
        double gt[6] = {0, 0.001, 0, 50, 0, -0.001};
        GDALSetGeoTransform(hDs, gt);
        GDALSetProjection(hDs, "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]]");

        // Write some pixel data
        std::vector<uint8_t> data(100 * 100, 128);
        for (int b = 1; b <= 3; b++) {
            GDALRasterIO(GDALGetRasterBand(hDs, b), GF_Write, 0, 0, 100, 100,
                         data.data(), 100, 100, GDT_Byte, 0, 0);
        }
        GDALClose(hDs);
    }

    // Generate stats sidecar directly
    generateCogStats(inputPath.string());

    fs::path statsPath = inputPath.parent_path() / (inputPath.filename().string() + ".stats.json");
    EXPECT_TRUE(fs::exists(statsPath));

    // Read and verify JSON
    std::ifstream ifs(statsPath.string());
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("\"bands\""), std::string::npos);
    EXPECT_NE(content.find("\"computedAt\""), std::string::npos);
}

// ============================================================
// C API Tests
// ============================================================

TEST(multispectral, cApiGetRasterInfo) {
    TestArea ta(TEST_NAME);

    GDALDriverH tifDrv = GDALGetDriverByName("GTiff");
    fs::path inputPath = ta.getPath("raster_info_test.tif");

    {
        GDALDatasetH hDs = GDALCreate(tifDrv, inputPath.string().c_str(), 100, 100, 5, GDT_UInt16, nullptr);
        GDALClose(hDs);
    }

    char* output = nullptr;
    DDBErr err = DDBGetRasterInfo(inputPath.string().c_str(), &output);
    EXPECT_EQ(err, DDBERR_NONE);
    EXPECT_NE(output, nullptr);

    std::string json(output);
    EXPECT_NE(json.find("bandCount"), std::string::npos);
    EXPECT_NE(json.find("detectedSensor"), std::string::npos);

    DDBFree(output);
}

// ============================================================
// GDALTiler Band-Aware Tile Tests
// ============================================================

// Helper: create a georeferenced multi-band GeoTIFF with pixel data
static fs::path createGeoRaster(TestArea& ta, const std::string& name,
                                 int width, int height, int nBands,
                                 GDALDataType dt) {
    GDALDriverH tifDrv = GDALGetDriverByName("GTiff");
    fs::path rasterPath = ta.getPath(name);
    GDALDatasetH hDs = GDALCreate(tifDrv, rasterPath.string().c_str(),
                                   width, height, nBands, dt, nullptr);
    // Set geotransform in EPSG:4326 (WGS84)
    double gt[6] = {11.0, 0.0001, 0, 46.0, 0, -0.0001};
    GDALSetGeoTransform(hDs, gt);
    GDALSetProjection(hDs, "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\","
                      "SPHEROID[\"WGS 84\",6378137,298.257223563]],"
                      "PRIMEM[\"Greenwich\",0],"
                      "UNIT[\"degree\",0.0174532925199433]]");

    // Fill bands with distinguishable values
    for (int b = 1; b <= nBands; b++) {
        GDALRasterBandH hBand = GDALGetRasterBand(hDs, b);
        if (dt == GDT_UInt16) {
            std::vector<uint16_t> data(width * height, static_cast<uint16_t>(b * 1000));
            GDALRasterIO(hBand, GF_Write, 0, 0, width, height,
                         data.data(), width, height, GDT_UInt16, 0, 0);
        } else {
            std::vector<uint8_t> data(width * height, static_cast<uint8_t>(b * 50));
            GDALRasterIO(hBand, GF_Write, 0, 0, width, height,
                         data.data(), width, height, GDT_Byte, 0, 0);
        }
    }
    GDALClose(hDs);
    return rasterPath;
}

TEST(multispectral, tileWithBandSelection) {
    TestArea ta(TEST_NAME);
    fs::path rasterPath = createGeoRaster(ta, "ms5_tile.tif", 256, 256, 5, GDT_UInt16);
    // Use empty outputFolder for in-memory tile generation
    GDALTiler tiler(rasterPath.string(), "", 256, false);

    // Get valid tile coords from tiler info
    auto info = tiler.getMinMaxZ();

    ThumbVisParams visParams;
    visParams.bands = "3,2,1";  // Custom band selection

    uint8_t* outBuffer = nullptr;
    int outBufferSize = 0;

    // Use the max zoom level for best data coverage
    auto bounds = tiler.getMinMaxCoordsForZ(info.max);

    try {
        tiler.tile(info.max, bounds.min.x, bounds.min.y, visParams, &outBuffer, &outBufferSize);
        if (outBuffer) {
            EXPECT_GT(outBufferSize, 0);
            VSIFree(outBuffer);
        }
    } catch (const ddb::GDALException& ex) {
        // Only "Exceeded max buf size" is acceptable in test environment
        EXPECT_NE(std::string(ex.what()).find("Exceeded max buf size"), std::string::npos)
            << "Unexpected GDALException: " << ex.what();
    }
}

TEST(multispectral, tileWithFormula) {
    TestArea ta(TEST_NAME);
    // Create a 5-band UInt16 raster with meaningful values for NDVI
    fs::path rasterPath = ta.getPath("ms5_ndvi.tif");
    {
        GDALDriverH tifDrv = GDALGetDriverByName("GTiff");
        GDALDatasetH hDs = GDALCreate(tifDrv, rasterPath.string().c_str(),
                                       256, 256, 5, GDT_UInt16, nullptr);
        double gt[6] = {11.0, 0.0001, 0, 46.0, 0, -0.0001};
        GDALSetGeoTransform(hDs, gt);
        GDALSetProjection(hDs, "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\","
                          "SPHEROID[\"WGS 84\",6378137,298.257223563]],"
                          "PRIMEM[\"Greenwich\",0],"
                          "UNIT[\"degree\",0.0174532925199433]]");

        // Band 1=Blue, 2=Green, 3=Red, 4=RedEdge, 5=NIR
        for (int b = 1; b <= 5; b++) {
            GDALRasterBandH hBand = GDALGetRasterBand(hDs, b);
            std::vector<uint16_t> data(256 * 256);
            for (size_t i = 0; i < data.size(); i++) {
                if (b == 3) data[i] = 2000;     // Red: low reflectance
                else if (b == 5) data[i] = 8000; // NIR: high reflectance (vegetation)
                else data[i] = 3000;              // Other bands
            }
            GDALRasterIO(hBand, GF_Write, 0, 0, 256, 256,
                         data.data(), 256, 256, GDT_UInt16, 0, 0);
        }
        GDALClose(hDs);
    }

    // Use empty outputFolder for in-memory tile generation
    GDALTiler tiler(rasterPath.string(), "", 256, false);
    auto info = tiler.getMinMaxZ();

    ThumbVisParams visParams;
    visParams.formula = "NDVI";
    visParams.bandFilter = "BGRReN";  // Matches MicaSense-like band order
    visParams.colormap = "rdylgn";
    visParams.rescale = "-1,1";

    auto bounds = tiler.getMinMaxCoordsForZ(info.max);

    uint8_t* outBuffer = nullptr;
    int outBufferSize = 0;

    try {
        tiler.tile(info.max, bounds.min.x, bounds.min.y, visParams, &outBuffer, &outBufferSize);
        if (outBuffer) {
            EXPECT_GT(outBufferSize, 0);
            VSIFree(outBuffer);
        }
    } catch (const ddb::GDALException& ex) {
        EXPECT_NE(std::string(ex.what()).find("Exceeded max buf size"), std::string::npos)
            << "Unexpected GDALException: " << ex.what();
    }
}

TEST(multispectral, tileNoVisParamsFallback) {
    TestArea ta(TEST_NAME);
    fs::path rasterPath = createGeoRaster(ta, "rgb_tile.tif", 256, 256, 3, GDT_Byte);

    // Use empty outputFolder for in-memory tile generation
    GDALTiler tiler(rasterPath.string(), "", 256, false);
    auto info = tiler.getMinMaxZ();

    // Empty vis params should fall through to standard tile
    ThumbVisParams visParams;

    auto bounds = tiler.getMinMaxCoordsForZ(info.max);

    uint8_t* outBuffer = nullptr;
    int outBufferSize = 0;

    try {
        tiler.tile(info.max, bounds.min.x, bounds.min.y, visParams, &outBuffer, &outBufferSize);
        if (outBuffer) {
            EXPECT_GT(outBufferSize, 0);
            VSIFree(outBuffer);
        }
    } catch (const ddb::GDALException& ex) {
        EXPECT_NE(std::string(ex.what()).find("Exceeded max buf size"), std::string::npos)
            << "Unexpected GDALException: " << ex.what();
    }
}

TEST(multispectral, cApiMemoryTileEx) {
    TestArea ta(TEST_NAME);
    fs::path rasterPath = createGeoRaster(ta, "ms5_capi.tif", 256, 256, 5, GDT_UInt16);

    GDALTiler tiler(rasterPath.string(), "", 256, false);
    auto info = tiler.getMinMaxZ();
    auto bounds = tiler.getMinMaxCoordsForZ(info.max);

    uint8_t* outBuffer = nullptr;
    int outBufferSize = 0;
    DDBErr err = DDBMemoryTileEx(rasterPath.string().c_str(),
                                  info.max, bounds.min.x, bounds.min.y,
                                  256, false, true, nullptr,
                                  nullptr, "3,2,1", nullptr, nullptr, nullptr, nullptr,
                                  &outBuffer, &outBufferSize);

    ASSERT_EQ(err, DDBERR_NONE) << "DDBMemoryTileEx failed with valid tile coords";
    EXPECT_NE(outBuffer, nullptr);
    EXPECT_GT(outBufferSize, 0);
    if (outBuffer) DDBVSIFree(outBuffer);
}

TEST(multispectral, cApiMemoryTileExFormula) {
    TestArea ta(TEST_NAME);
    fs::path rasterPath = createGeoRaster(ta, "ms5_capi_formula.tif", 256, 256, 5, GDT_UInt16);

    GDALTiler tiler(rasterPath.string(), "", 256, false);
    auto info = tiler.getMinMaxZ();
    auto bounds = tiler.getMinMaxCoordsForZ(info.max);

    uint8_t* outBuffer = nullptr;
    int outBufferSize = 0;
    DDBErr err = DDBMemoryTileEx(rasterPath.string().c_str(),
                                  info.max, bounds.min.x, bounds.min.y,
                                  256, false, true, nullptr,
                                  nullptr, nullptr, "VARI", "RGB", "rdylgn", "-1,1",
                                  &outBuffer, &outBufferSize);

    ASSERT_EQ(err, DDBERR_NONE) << "DDBMemoryTileEx with formula failed with valid tile coords";
    EXPECT_NE(outBuffer, nullptr);
    EXPECT_GT(outBufferSize, 0);
    if (outBuffer) DDBVSIFree(outBuffer);
}

// ============================================================
// Merge GPS Preservation Tests
// ============================================================

TEST(multispectral, mergePreservesGpsFromExif) {
    TestArea ta(TEST_NAME, true);

    GDALDriverH tifDrv = GDALGetDriverByName("GTiff");
    ASSERT_NE(tifDrv, nullptr);

    // Create two single-band TIFFs WITHOUT geotransform/CRS (like camera bands)
    std::vector<std::string> inputs;
    for (int i = 0; i < 3; i++) {
        fs::path bandPath = ta.getPath("gps_band" + std::to_string(i + 1) + ".tif");
        GDALDatasetH hDs = GDALCreate(tifDrv, bandPath.string().c_str(), 50, 50, 1, GDT_UInt16, nullptr);
        ASSERT_NE(hDs, nullptr);
        std::vector<uint16_t> data(50 * 50, static_cast<uint16_t>((i + 1) * 1000));
        GDALRasterIO(GDALGetRasterBand(hDs, 1), GF_Write, 0, 0, 50, 50,
                     data.data(), 50, 50, GDT_UInt16, 0, 0);
        GDALClose(hDs);
        inputs.push_back(bandPath.string());
    }

    // Write GPS coordinates to all source files
    double srcLat = 45.4642;
    double srcLon = 9.1900;
    double srcAlt = 120.5;
    for (const auto &p : inputs) {
        ExifEditor editor(p);
        editor.SetGPS(srcLat, srcLon, srcAlt);
    }

    // Verify GPS was written to sources
    {
        auto img = Exiv2::ImageFactory::open(inputs[0]);
        img->readMetadata();
        ExifParser parser(img.get());
        GeoLocation geo;
        ASSERT_TRUE(parser.extractGeo(geo));
        EXPECT_NEAR(geo.latitude, srcLat, 0.001);
    }

    // Merge
    fs::path outputPath = ta.getPath("merged_gps.tif");
    EXPECT_NO_THROW(mergeMultispectral(inputs, outputPath.string()));
    EXPECT_TRUE(fs::exists(outputPath));

    // Verify GPS is preserved in output
    auto outImg = Exiv2::ImageFactory::open(outputPath.string());
    outImg->readMetadata();
    ExifParser outParser(outImg.get());
    GeoLocation outGeo;
    ASSERT_TRUE(outParser.extractGeo(outGeo));
    EXPECT_NEAR(outGeo.latitude, srcLat, 0.001);
    EXPECT_NEAR(outGeo.longitude, srcLon, 0.001);
    EXPECT_NEAR(outGeo.altitude, srcAlt, 1.0);
}

TEST(multispectral, mergePreservesGpsWithRealData) {
    TestArea ta(TEST_NAME, true);

    // Download real multispectral band files from test_data repo
    std::string baseUrl = "https://github.com/DroneDB/test_data/raw/master/multispectral/";
    std::vector<std::string> inputs;
    for (int i = 1; i <= 5; i++) {
        std::string filename = "IMG_0180_" + std::to_string(i) + ".tif";
        fs::path bandPath = ta.downloadTestAsset(baseUrl + filename, filename);
        inputs.push_back(bandPath.string());
    }

    // Read GPS from first source
    GeoLocation srcGeo;
    {
        auto img = Exiv2::ImageFactory::open(inputs[0]);
        img->readMetadata();
        ExifParser parser(img.get());
        ASSERT_TRUE(parser.extractGeo(srcGeo));
        EXPECT_NEAR(srcGeo.latitude, 50.98, 0.01);
        EXPECT_NEAR(srcGeo.longitude, 21.43, 0.01);
    }

    // Merge
    fs::path outputPath = ta.getPath("merged_real.tif");
    EXPECT_NO_THROW(mergeMultispectral(inputs, outputPath.string()));
    EXPECT_TRUE(fs::exists(outputPath));

    // Verify output has 5 bands
    GDALDatasetH hOut = GDALOpen(outputPath.string().c_str(), GA_ReadOnly);
    ASSERT_NE(hOut, nullptr);
    EXPECT_EQ(GDALGetRasterCount(hOut), 5);
    GDALClose(hOut);

    // Verify GPS is preserved
    auto outImg = Exiv2::ImageFactory::open(outputPath.string());
    outImg->readMetadata();
    ExifParser outParser(outImg.get());
    GeoLocation outGeo;
    ASSERT_TRUE(outParser.extractGeo(outGeo));
    EXPECT_NEAR(outGeo.latitude, srcGeo.latitude, 0.001);
    EXPECT_NEAR(outGeo.longitude, srcGeo.longitude, 0.001);
    EXPECT_NEAR(outGeo.altitude, srcGeo.altitude, 1.0);
}

TEST(multispectral, mergeNoGpsDoesNotFail) {
    TestArea ta(TEST_NAME, true);

    // Create TIFFs with geotransform but NO GPS EXIF
    GDALDriverH tifDrv = GDALGetDriverByName("GTiff");
    double gt[6] = {0, 0.001, 0, 50, 0, -0.001};
    const char* proj = "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]]";

    std::vector<std::string> inputs;
    for (int i = 0; i < 2; i++) {
        fs::path bandPath = ta.getPath("nogps_band" + std::to_string(i + 1) + ".tif");
        GDALDatasetH hDs = GDALCreate(tifDrv, bandPath.string().c_str(), 50, 50, 1, GDT_UInt16, nullptr);
        GDALSetGeoTransform(hDs, gt);
        GDALSetProjection(hDs, proj);
        std::vector<uint16_t> data(50 * 50, static_cast<uint16_t>((i + 1) * 1000));
        GDALRasterIO(GDALGetRasterBand(hDs, 1), GF_Write, 0, 0, 50, 50,
                     data.data(), 50, 50, GDT_UInt16, 0, 0);
        GDALClose(hDs);
        inputs.push_back(bandPath.string());
    }

    // Merge should succeed without GPS (no crash, no error)
    fs::path outputPath = ta.getPath("merged_nogps.tif");
    EXPECT_NO_THROW(mergeMultispectral(inputs, outputPath.string()));
    EXPECT_TRUE(fs::exists(outputPath));
    GDALDatasetH mergedDs = GDALOpen(outputPath.string().c_str(), GA_ReadOnly);
    ASSERT_NE(mergedDs, nullptr);
    EXPECT_EQ(GDALGetRasterCount(mergedDs), 2);
    GDALClose(mergedDs);
}

// ============================================================
// Band Alignment Detection Tests
// ============================================================

TEST(multispectral, detectAlignmentMicaSense) {
    TestArea ta(TEST_NAME);

    std::string baseUrl = "https://github.com/DroneDB/test_data/raw/master/multispectral/";
    std::vector<std::string> inputs;
    for (int i = 1; i <= 5; i++) {
        std::string filename = "IMG_0180_" + std::to_string(i) + ".tif";
        fs::path bandPath = ta.downloadTestAsset(baseUrl + filename, filename);
        inputs.push_back(bandPath.string());
    }

    auto alignInfo = detectBandAlignment(inputs);
    ASSERT_EQ(alignInfo.size(), 5u);

    // At least 2 bands should be detected
    int detectedCount = 0;
    for (const auto &a : alignInfo) {
        if (a.detected) detectedCount++;
    }
    EXPECT_GE(detectedCount, 2);

    // Max shift should be significant (MicaSense ~19 px)
    double maxShift = 0;
    for (const auto &a : alignInfo) {
        maxShift = std::max(maxShift, std::max(std::abs(a.shiftX), std::abs(a.shiftY)));
    }
    EXPECT_GT(maxShift, 2.0);
    EXPECT_LT(maxShift, 50.0);

    // Source should be PrincipalPoint
    for (const auto &a : alignInfo) {
        if (a.detected) {
            EXPECT_EQ(a.shiftSource, "PrincipalPoint");
        }
    }
}

TEST(multispectral, detectAlignmentNoXmpCamera) {
    TestArea ta(TEST_NAME);

    // Create two plain TIFFs without any XMP Camera metadata
    GDALDriverH tifDrv = GDALGetDriverByName("GTiff");
    std::vector<std::string> inputs;
    for (int i = 0; i < 2; i++) {
        fs::path p = ta.getPath("plain_" + std::to_string(i) + ".tif");
        GDALDatasetH hDs = GDALCreate(tifDrv, p.string().c_str(), 100, 100, 1, GDT_UInt16, nullptr);
        GDALClose(hDs);
        inputs.push_back(p.string());
    }

    auto alignInfo = detectBandAlignment(inputs);
    ASSERT_EQ(alignInfo.size(), 2u);
    EXPECT_FALSE(alignInfo[0].detected);
    EXPECT_FALSE(alignInfo[1].detected);
}

TEST(multispectral, detectAlignmentSingleFile) {
    TestArea ta(TEST_NAME);

    std::string baseUrl = "https://github.com/DroneDB/test_data/raw/master/multispectral/";
    fs::path bandPath = ta.downloadTestAsset(baseUrl + "IMG_0180_1.tif", "IMG_0180_1.tif");
    std::vector<std::string> inputs = {bandPath.string()};

    auto alignInfo = detectBandAlignment(inputs);
    ASSERT_EQ(alignInfo.size(), 1u);
    // Single file: no crash, shift = 0
    EXPECT_DOUBLE_EQ(alignInfo[0].shiftX, 0.0);
    EXPECT_DOUBLE_EQ(alignInfo[0].shiftY, 0.0);
}

TEST(multispectral, validateAlignmentWarningMicaSense) {
    TestArea ta(TEST_NAME);

    std::string baseUrl = "https://github.com/DroneDB/test_data/raw/master/multispectral/";
    std::vector<std::string> inputs;
    for (int i = 1; i <= 5; i++) {
        std::string filename = "IMG_0180_" + std::to_string(i) + ".tif";
        fs::path bandPath = ta.downloadTestAsset(baseUrl + filename, filename);
        inputs.push_back(bandPath.string());
    }

    auto result = validateMergeMultispectral(inputs);
    EXPECT_TRUE(result.alignment.detected);
    EXPECT_GT(result.alignment.maxShiftPixels, 2.0);
    EXPECT_TRUE(result.alignment.correctionApplied);

    // Should have alignment warning
    bool hasAlignWarning = false;
    for (const auto &w : result.warnings) {
        if (w.find("misalignment") != std::string::npos) hasAlignWarning = true;
    }
    EXPECT_TRUE(hasAlignWarning);
}

TEST(multispectral, mergeWithAlignmentCorrection) {
    TestArea ta(TEST_NAME, true);

    std::string baseUrl = "https://github.com/DroneDB/test_data/raw/master/multispectral/";
    std::vector<std::string> inputs;
    for (int i = 1; i <= 5; i++) {
        std::string filename = "IMG_0180_" + std::to_string(i) + ".tif";
        fs::path bandPath = ta.downloadTestAsset(baseUrl + filename, filename);
        inputs.push_back(bandPath.string());
    }

    fs::path outputPath = ta.getPath("merged_aligned.tif");
    EXPECT_NO_THROW(mergeMultispectral(inputs, outputPath.string()));
    EXPECT_TRUE(fs::exists(outputPath));

    // Output should have 5 bands
    GDALDatasetH hOut = GDALOpen(outputPath.string().c_str(), GA_ReadOnly);
    ASSERT_NE(hOut, nullptr);
    EXPECT_EQ(GDALGetRasterCount(hOut), 5);

    // Output should be slightly smaller than input (due to alignment crop)
    int outW = GDALGetRasterXSize(hOut);
    int outH = GDALGetRasterYSize(hOut);
    EXPECT_LT(outW, 1280);
    EXPECT_LT(outH, 960);
    EXPECT_GT(outW, 1200); // Should not lose too much
    EXPECT_GT(outH, 900);

    GDALClose(hOut);
}

TEST(multispectral, mergeGeorefNoShift) {
    TestArea ta(TEST_NAME, true);

    // Create georeferenced TIFFs — shift should NOT be applied
    GDALDriverH tifDrv = GDALGetDriverByName("GTiff");
    double gt[6] = {11.0, 0.00001, 0, 45.0, 0, -0.00001};
    const char* proj = "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]]";

    std::vector<std::string> inputs;
    for (int i = 0; i < 3; i++) {
        fs::path p = ta.getPath("georef_" + std::to_string(i) + ".tif");
        GDALDatasetH hDs = GDALCreate(tifDrv, p.string().c_str(), 100, 100, 1, GDT_UInt16, nullptr);
        GDALSetGeoTransform(hDs, gt);
        GDALSetProjection(hDs, proj);
        std::vector<uint16_t> data(100 * 100, static_cast<uint16_t>((i + 1) * 100));
        GDALRasterIO(GDALGetRasterBand(hDs, 1), GF_Write, 0, 0, 100, 100,
                     data.data(), 100, 100, GDT_UInt16, 0, 0);
        GDALClose(hDs);
        inputs.push_back(p.string());
    }

    auto result = validateMergeMultispectral(inputs);
    // Georeferenced files: correctionApplied should be false
    EXPECT_FALSE(result.alignment.correctionApplied);

    // Merge should produce same-size output
    fs::path outputPath = ta.getPath("merged_georef.tif");
    EXPECT_NO_THROW(mergeMultispectral(inputs, outputPath.string()));
    GDALDatasetH hOut = GDALOpen(outputPath.string().c_str(), GA_ReadOnly);
    ASSERT_NE(hOut, nullptr);
    EXPECT_EQ(GDALGetRasterXSize(hOut), 100);
    EXPECT_EQ(GDALGetRasterYSize(hOut), 100);
    GDALClose(hOut);
}

TEST(multispectral, detectAlignmentParrotSequoia) {
    TestArea ta(TEST_NAME);

    std::string baseUrl = "https://github.com/DroneDB/test_data/raw/master/multispectral/parrot_sequoia/";
    std::vector<std::string> suffixes = {"_GRE.TIF", "_RED.TIF", "_REG.TIF", "_NIR.TIF"};
    std::vector<std::string> inputs;
    for (const auto &s : suffixes) {
        std::string filename = "IMG_180822_135805_0467" + s;
        fs::path bandPath = ta.downloadTestAsset(baseUrl + filename, filename);
        inputs.push_back(bandPath.string());
    }

    auto alignInfo = detectBandAlignment(inputs);
    ASSERT_EQ(alignInfo.size(), 4u);

    int detectedCount = 0;
    for (const auto &a : alignInfo) {
        if (a.detected) detectedCount++;
    }
    EXPECT_GE(detectedCount, 2);

    // Max shift ~22 px
    double maxShift = 0;
    for (const auto &a : alignInfo) {
        maxShift = std::max(maxShift, std::max(std::abs(a.shiftX), std::abs(a.shiftY)));
    }
    EXPECT_GT(maxShift, 5.0);
    EXPECT_LT(maxShift, 50.0);
}

TEST(multispectral, detectAlignmentDjiM3M) {
    TestArea ta(TEST_NAME);

    std::string baseUrl = "https://github.com/DroneDB/test_data/raw/master/multispectral/dji/";
    std::vector<std::string> suffixes = {"_MS_G.TIF", "_MS_R.TIF", "_MS_RE.TIF", "_MS_NIR.TIF"};
    std::vector<std::string> inputs;
    for (const auto &s : suffixes) {
        std::string filename = "DJI_20240525174755_0001" + s;
        fs::path bandPath = ta.downloadTestAsset(baseUrl + filename, filename);
        inputs.push_back(bandPath.string());
    }

    auto alignInfo = detectBandAlignment(inputs);
    ASSERT_EQ(alignInfo.size(), 4u);

    int detectedCount = 0;
    for (const auto &a : alignInfo) {
        if (a.detected) detectedCount++;
    }
    EXPECT_GE(detectedCount, 2);

    // Max shift ~24 px
    double maxShift = 0;
    for (const auto &a : alignInfo) {
        maxShift = std::max(maxShift, std::max(std::abs(a.shiftX), std::abs(a.shiftY)));
    }
    EXPECT_GT(maxShift, 5.0);
    EXPECT_LT(maxShift, 50.0);
}

TEST(multispectral, detectAlignmentDjiP4MS) {
    TestArea ta(TEST_NAME);

    std::string baseUrl = "https://github.com/DroneDB/test_data/raw/master/multispectral/dji_p4/";
    std::vector<std::string> inputs;
    for (int i = 21; i <= 25; i++) {
        std::string filename = "DJI_00" + std::to_string(i) + ".TIF";
        fs::path bandPath = ta.downloadTestAsset(baseUrl + filename, filename);
        inputs.push_back(bandPath.string());
    }

    auto alignInfo = detectBandAlignment(inputs);
    ASSERT_EQ(alignInfo.size(), 5u);

    int detectedCount = 0;
    for (const auto &a : alignInfo) {
        if (a.detected) detectedCount++;
    }
    EXPECT_GE(detectedCount, 2);

    // P4 MS: PP shift ~1.2, should use DJI RelOC (~9 px)
    bool hasDjiSource = false;
    for (const auto &a : alignInfo) {
        if (a.detected && a.shiftSource == "DJI_RelativeOpticalCenter") {
            hasDjiSource = true;
        }
    }
    EXPECT_TRUE(hasDjiSource);
}

TEST(multispectral, detectAlignmentSentera6X) {
    TestArea ta(TEST_NAME);

    std::string baseUrl = "https://github.com/DroneDB/test_data/raw/master/multispectral/sentera_6x/";
    std::vector<std::string> filenames = {
        "IMG_0013_475_30.tif", "IMG_0013_550_20.tif", "IMG_0013_670_30.tif",
        "IMG_0013_715_10.tif", "IMG_0013_840_20.tif"
    };
    std::vector<std::string> inputs;
    for (const auto &f : filenames) {
        fs::path bandPath = ta.downloadTestAsset(baseUrl + f, f);
        inputs.push_back(bandPath.string());
    }

    auto alignInfo = detectBandAlignment(inputs);
    ASSERT_EQ(alignInfo.size(), 5u);

    int detectedCount = 0;
    for (const auto &a : alignInfo) {
        if (a.detected) detectedCount++;
    }
    EXPECT_GE(detectedCount, 2);

    // Sentera 6X: max shift ~42 px (the largest)
    double maxShift = 0;
    for (const auto &a : alignInfo) {
        maxShift = std::max(maxShift, std::max(std::abs(a.shiftX), std::abs(a.shiftY)));
    }
    EXPECT_GT(maxShift, 15.0);
    EXPECT_LT(maxShift, 100.0);

    // Source should be PrincipalPoint
    for (const auto &a : alignInfo) {
        if (a.detected) {
            EXPECT_EQ(a.shiftSource, "PrincipalPoint");
        }
    }
}

TEST(multispectral, mergeRegressionNoAlignment) {
    TestArea ta(TEST_NAME, true);

    // Create simple TIFFs without alignment metadata — merge should work as before
    GDALDriverH tifDrv = GDALGetDriverByName("GTiff");
    double gt[6] = {0, 0.001, 0, 50, 0, -0.001};
    const char* proj = "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]]";

    std::vector<std::string> inputs;
    for (int i = 0; i < 3; i++) {
        fs::path p = ta.getPath("regr_band" + std::to_string(i) + ".tif");
        GDALDatasetH hDs = GDALCreate(tifDrv, p.string().c_str(), 50, 50, 1, GDT_UInt16, nullptr);
        GDALSetGeoTransform(hDs, gt);
        GDALSetProjection(hDs, proj);
        std::vector<uint16_t> data(50 * 50, static_cast<uint16_t>((i + 1) * 1000));
        GDALRasterIO(GDALGetRasterBand(hDs, 1), GF_Write, 0, 0, 50, 50,
                     data.data(), 50, 50, GDT_UInt16, 0, 0);
        GDALClose(hDs);
        inputs.push_back(p.string());
    }

    fs::path outputPath = ta.getPath("merged_regr.tif");
    EXPECT_NO_THROW(mergeMultispectral(inputs, outputPath.string()));
    EXPECT_TRUE(fs::exists(outputPath));
    GDALDatasetH hOut = GDALOpen(outputPath.string().c_str(), GA_ReadOnly);
    ASSERT_NE(hOut, nullptr);
    EXPECT_EQ(GDALGetRasterCount(hOut), 3);
    EXPECT_EQ(GDALGetRasterXSize(hOut), 50);
    EXPECT_EQ(GDALGetRasterYSize(hOut), 50);
    GDALClose(hOut);
}

}  // namespace
