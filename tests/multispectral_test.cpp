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
#include "gdal_inc.h"
#include "mio.h"
#include "exceptions.h"

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

}  // namespace
