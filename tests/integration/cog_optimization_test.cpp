/* Test for COG optimization functionality */
#include "gtest/gtest.h"
#include "cog_utils.h"
#include "cog.h"
#include "logger.h"
#include "testarea.h"
#include "testfs.h"
#include "ddb.h"
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "gdal_inc.h"
#include "user_open_child.h"

namespace {

class CogOptimizationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Initialize DDB process
    DDBRegisterProcess(false);
  }
};

USER_OPEN_TEST_F(CogOptimizationTest, TestCogDetection) {
    TestArea ta("CogOptimizationTest");

    // Download test files from DroneDB test data repository
    fs::path cogFile = ta.downloadTestAsset(
        testDataUrl("ortho/brighton-beach-cog.tif"),
        "brighton-beach-cog.tif"
    );

    fs::path nonCogFile = ta.downloadTestAsset(
        testDataUrl("ortho/brighton-beach.tif"),
        "brighton-beach.tif"
    );

    // Test COG detection
    EXPECT_TRUE(ddb::isOptimizedCog(cogFile.string()))
        << "COG file should be detected as optimized";

    EXPECT_FALSE(ddb::isOptimizedCog(nonCogFile.string()))
        << "Non-COG file should not be detected as optimized";
}

USER_OPEN_TEST_F(CogOptimizationTest, TestBuildCogOptimization) {
    TestArea ta("CogOptimizationTest");

    // Download COG test file: 1913 x 1860 RGB pixels of 7.3 cm in Web Mercator, YCbCr JPEG of
    // quality 90 with a mask, not on the tile grid
    fs::path cogFile = ta.downloadTestAsset(
        testDataUrl("ortho/brighton-beach-cog.tif"),
        "brighton-beach-cog.tif"
    );

    // Test building COG from already optimized file
    fs::path outputPath = ta.getPath("test-output-cog.tif");

    EXPECT_NO_THROW({
        ddb::buildCog(cogFile.string(), outputPath.string());
    });

    // Verify output file exists
    ASSERT_TRUE(fs::exists(outputPath)) << "Output COG file should exist";

    // Verify output is also a valid COG
    EXPECT_TRUE(ddb::isOptimizedCog(outputPath.string()))
        << "Output should also be detected as optimized COG";

    // Encoded again at its own grid, not resampled to the tile grid and not copied
    std::ifstream inputBytes(cogFile, std::ios::binary), outputBytes(outputPath, std::ios::binary);
    const std::string inputContent((std::istreambuf_iterator<char>(inputBytes)), std::istreambuf_iterator<char>());
    const std::string outputContent((std::istreambuf_iterator<char>(outputBytes)), std::istreambuf_iterator<char>());
    EXPECT_NE(outputContent, inputContent) << "a file of a user is never copied";

    GDALDatasetH input = GDALOpen(cogFile.string().c_str(), GA_ReadOnly);
    GDALDatasetH output = GDALOpen(outputPath.string().c_str(), GA_ReadOnly);
    ASSERT_NE(input, nullptr);
    ASSERT_NE(output, nullptr);
    const int width = GDALGetRasterXSize(input);
    const int height = GDALGetRasterYSize(input);
    EXPECT_EQ(width, 1913);
    EXPECT_EQ(height, 1860);
    ASSERT_EQ(GDALGetRasterXSize(output), width);
    ASSERT_EQ(GDALGetRasterYSize(output), height);
    double gtIn[6] = {0}, gtOut[6] = {0};
    GDALGetGeoTransform(input, gtIn);
    GDALGetGeoTransform(output, gtOut);
    EXPECT_EQ(std::vector<double>(gtOut, gtOut + 6), std::vector<double>(gtIn, gtIn + 6));
    EXPECT_TRUE(OSRIsSame(GDALGetSpatialRef(output), GDALGetSpatialRef(input)));
    ASSERT_EQ(GDALGetRasterCount(output), GDALGetRasterCount(input));

    // The mask is lossless and equal. The input is JPEG, so the output is JPEG of the same
    // quality: decoding and encoding it again moves a pixel by a small fraction of a level on
    // average (a few levels at most on sharp edges), where a resampling or a lower quality would
    // move it by several.
    const size_t count = static_cast<size_t>(width) * height;
    std::vector<uint8_t> in(count), out(count);
    GDALRasterBandH maskIn = GDALGetMaskBand(GDALGetRasterBand(input, 1));
    GDALRasterBandH maskOut = GDALGetMaskBand(GDALGetRasterBand(output, 1));
    ASSERT_EQ(GDALRasterIO(maskIn, GF_Read, 0, 0, width, height, in.data(), width, height, GDT_Byte, 0, 0), CE_None);
    ASSERT_EQ(GDALRasterIO(maskOut, GF_Read, 0, 0, width, height, out.data(), width, height, GDT_Byte, 0, 0), CE_None);
    EXPECT_TRUE(in == out) << "the mask is the one of the input";
    for (int b = 1; b <= GDALGetRasterCount(input); b++) {
        SCOPED_TRACE(b);
        GDALRasterBandH bandIn = GDALGetRasterBand(input, b);
        GDALRasterBandH bandOut = GDALGetRasterBand(output, b);
        EXPECT_EQ(GDALGetRasterDataType(bandOut), GDALGetRasterDataType(bandIn));
        EXPECT_EQ(GDALGetRasterColorInterpretation(bandOut), GDALGetRasterColorInterpretation(bandIn));
        ASSERT_EQ(GDALRasterIO(bandIn, GF_Read, 0, 0, width, height, in.data(), width, height, GDT_Byte, 0, 0), CE_None);
        ASSERT_EQ(GDALRasterIO(bandOut, GF_Read, 0, 0, width, height, out.data(), width, height, GDT_Byte, 0, 0), CE_None);
        double totalDifference = 0;
        for (size_t i = 0; i < count; i++)
            totalDifference += std::abs(static_cast<int>(in[i]) - static_cast<int>(out[i]));
        EXPECT_LT(totalDifference / static_cast<double>(count), 0.5) << "mean difference of a pixel";
    }
    GDALClose(output);
    GDALClose(input);
}

USER_OPEN_TEST_F(CogOptimizationTest, TestBuildCogFromNonOptimized) {
    TestArea ta("CogOptimizationTest");

    // Download non-COG test file
    fs::path nonCogFile = ta.downloadTestAsset(
        testDataUrl("ortho/brighton-beach.tif"),
        "brighton-beach.tif"
    );

    // Test building COG from non-optimized file (should do full rebuild)
    fs::path outputPath = ta.getPath("test-output-from-noncog.tif");

    EXPECT_NO_THROW({
        ddb::buildCog(nonCogFile.string(), outputPath.string());
    });

    // Verify output file exists
    EXPECT_TRUE(fs::exists(outputPath)) << "Output COG file should exist";

    // Verify output is a valid COG (after full rebuild)
    EXPECT_TRUE(ddb::isOptimizedCog(outputPath.string()))
        << "Output from non-COG should be optimized after rebuild";
}

USER_OPEN_TEST_F(CogOptimizationTest, TestInvalidFiles) {
    TestArea ta("CogOptimizationTest");

    // Test with non-existent file
    EXPECT_FALSE(ddb::isOptimizedCog(ta.getPath("non_existent_file.tif").string()))
        << "Non-existent file should return false";

    // Test buildCog with invalid input. The output goes to the test area, never to the
    // current folder (the folder of the test programs)
    const fs::path output = ta.getPath("output.tif");
    EXPECT_THROW({
        ddb::buildCog(ta.getPath("non_existent_input.tif").string(), output.string());
    }, std::exception) << "Should throw exception for non-existent input";
    EXPECT_FALSE(fs::exists(output));
}

} // namespace