/* Test edge-case georasters for COG conversion and tiling pipeline */
#include "gtest/gtest.h"
#include "cog.h"
#include "cog_utils.h"
#include "tilerhelper.h"
#include "logger.h"
#include "testarea.h"
#include "ddb.h"
#include "exceptions.h"
#include "gdal_inc.h"

#include <sstream>

namespace {

const std::string EDGE_BASE_URL =
    "https://github.com/DroneDB/test_data/raw/master/ortho/edge/";

class EdgeRasterTest : public ::testing::Test {
 protected:
    void SetUp() override {
        DDBRegisterProcess(false);
    }

    fs::path download(TestArea &ta, const std::string &filename) {
        return ta.downloadTestAsset(EDGE_BASE_URL + filename, filename);
    }

    // Verify the COG output is valid: exists, in EPSG:3857, stats.json present.
    // Note: isOptimizedCog() requires overviews which GDAL may not generate
    // for very small rasters, so we check projection directly instead.
    void validateCog(const fs::path &cogPath) {
        ASSERT_TRUE(fs::exists(cogPath)) << "COG output must exist: " << cogPath;

        // Verify it's in EPSG:3857
        GDALDatasetH hDs = GDALOpen(cogPath.string().c_str(), GA_ReadOnly);
        ASSERT_NE(hDs, nullptr) << "Cannot open COG output with GDAL";

        const char* projRef = GDALGetProjectionRef(hDs);
        ASSERT_NE(projRef, nullptr);
        ASSERT_GT(strlen(projRef), 0u) << "COG must have a projection";

        OGRSpatialReferenceH hSRS = OSRNewSpatialReference(projRef);
        OGRSpatialReferenceH h3857 = OSRNewSpatialReference(nullptr);
        OSRImportFromEPSG(h3857, 3857);
        EXPECT_TRUE(OSRIsSame(hSRS, h3857))
            << "COG output must be in EPSG:3857";
        OSRDestroySpatialReference(hSRS);
        OSRDestroySpatialReference(h3857);

        GDALClose(hDs);

        // Check stats sidecar
        fs::path statsPath = cogPath.parent_path() /
            (cogPath.filename().string() + ".stats.json");
        EXPECT_TRUE(fs::exists(statsPath))
            << "Stats sidecar must exist: " << statsPath;
    }

    // Run tiling on a COG and verify at least one tile is produced
    void validateTiling(TestArea &ta, const fs::path &cogPath,
                        const std::string &subdir) {
        fs::path tilesDir = ta.getFolder(subdir);

        std::ostringstream os;
        ASSERT_NO_THROW({
            ddb::TilerHelper::runTiler(cogPath, tilesDir, 256, false, os);
        }) << "Tiling must not throw";

        // Verify output is non-empty (tiles were generated)
        std::string output = os.str();
        EXPECT_FALSE(output.empty())
            << "Tiler should produce at least one tile";
    }

    // Full pipeline: buildCog -> validate COG -> tile -> validate tiles
    void runFullPipeline(const std::string &filename) {
        TestArea ta("EdgeRaster_" + filename);
        fs::path input = download(ta, filename);
        fs::path cogOutput = ta.getPath("output.tif");

        ASSERT_NO_THROW({
            ddb::buildCog(input.string(), cogOutput.string());
        }) << "buildCog must succeed for " << filename;

        validateCog(cogOutput);
        validateTiling(ta, cogOutput, "tiles");
    }
};

// --- Tests that expect buildCog to FAIL gracefully ---

TEST_F(EdgeRasterTest, NoCrs) {
    TestArea ta("EdgeRaster_NoCrs");
    fs::path input = download(ta, "gadas-world.tif");
    fs::path cogOutput = ta.getPath("output.tif");

    EXPECT_THROW({
        ddb::buildCog(input.string(), cogOutput.string());
    }, ddb::GDALException) << "buildCog must throw for input with no CRS";
}

TEST_F(EdgeRasterTest, InvalidEllipsoidCrs) {
    TestArea ta("EdgeRaster_InvalidEllipsoidCrs");
    fs::path input = download(ta, "GeogToWGS84GeoKey5.tif");
    fs::path cogOutput = ta.getPath("output.tif");

    // EPSG:7004 is an ellipsoid, not a proper CRS.
    // GDAL may or may not handle this; we just want no crash/segfault.
    try {
        ddb::buildCog(input.string(), cogOutput.string());
        // If it succeeds, validate
        if (fs::exists(cogOutput)) {
            validateCog(cogOutput);
            validateTiling(ta, cogOutput, "tiles");
        }
    } catch (const ddb::GDALException &) {
        // Graceful failure is acceptable
    }
}

// --- Full pipeline tests ---

TEST_F(EdgeRasterTest, PaletteColorUtm) {
    runFullPipeline("utm.tif");
}

TEST_F(EdgeRasterTest, RotatedGeoTransform) {
    runFullPipeline("umbra_mount_yasur.tiff");
}

TEST_F(EdgeRasterTest, NodataInfinity52Bands) {
    runFullPipeline("abetow-ERD2018-EBIRD_SCIENCE-20191109-a5cf4cb2_hr_2018_abundance_median.tiff");
}

TEST_F(EdgeRasterTest, NodataExtremeNegFloat) {
    runFullPipeline("nt_20201024_f18_nrt_s.tif");
}

TEST_F(EdgeRasterTest, NodataExtremePositive) {
    runFullPipeline("GA4886_VanderfordGlacier_2022_EGM2008_64m-epsg3031.cog");
}

TEST_F(EdgeRasterTest, PolarAntarctic) {
    runFullPipeline("vestfold.tif");
}

TEST_F(EdgeRasterTest, CogNsidc) {
    TestArea ta("EdgeRaster_CogNsidc");
    fs::path input = download(ta, "bremen_sea_ice_conc_2022_9_9.tif");

    // Already a COG but not in EPSG:3857
    EXPECT_FALSE(ddb::isOptimizedCog(input.string()))
        << "NSIDC COG should not be detected as optimized (not EPSG:3857)";

    fs::path cogOutput = ta.getPath("output.tif");
    ASSERT_NO_THROW({
        ddb::buildCog(input.string(), cogOutput.string());
    });
    validateCog(cogOutput);
    validateTiling(ta, cogOutput, "tiles");
}

TEST_F(EdgeRasterTest, CogAustralianAlbers) {
    TestArea ta("EdgeRaster_CogAustralianAlbers");
    fs::path input = download(ta, "ga_ls_tc_pc_cyear_3_x17y37_2022--P1Y_final_wet_pc_50_LQ.tif");

    EXPECT_FALSE(ddb::isOptimizedCog(input.string()))
        << "Australian Albers COG should not be detected as optimized";

    fs::path cogOutput = ta.getPath("output.tif");
    ASSERT_NO_THROW({
        ddb::buildCog(input.string(), cogOutput.string());
    });
    validateCog(cogOutput);
    validateTiling(ta, cogOutput, "tiles");
}

TEST_F(EdgeRasterTest, GlobalExtentDeflate) {
    // Cannot use runFullPipeline because the filename contains ".." which
    // TestArea rejects as path traversal.
    const std::string filename = "lcv_landuse.cropland_hyde_p_10km_s0..0cm_2016_v3.2.tif";
    TestArea ta("EdgeRaster_GlobalExtentDeflate");
    fs::path input = ta.downloadTestAsset(EDGE_BASE_URL + filename,
                                          "global_extent_deflate.tif");
    fs::path cogOutput = ta.getPath("output.tif");

    ASSERT_NO_THROW({
        ddb::buildCog(input.string(), cogOutput.string());
    }) << "buildCog must succeed for " << filename;

    validateCog(cogOutput);
    validateTiling(ta, cogOutput, "tiles");
}

TEST_F(EdgeRasterTest, TinyRasterCog) {
    runFullPipeline("gfw-azores.tif");
}

TEST_F(EdgeRasterTest, WebMercatorNonCog) {
    TestArea ta("EdgeRaster_WebMercatorNonCog");
    fs::path input = download(ta, "gadas.tif");

    // Already in EPSG:3857 coordinates but not a valid COG
    EXPECT_FALSE(ddb::isOptimizedCog(input.string()))
        << "Non-COG file should not be detected as optimized";

    fs::path cogOutput = ta.getPath("output.tif");
    ASSERT_NO_THROW({
        ddb::buildCog(input.string(), cogOutput.string());
    });
    validateCog(cogOutput);
    validateTiling(ta, cogOutput, "tiles");
}

TEST_F(EdgeRasterTest, StripGlobal) {
    runFullPipeline("nz_habitat_anticross_4326_1deg.tif");
}

TEST_F(EdgeRasterTest, Dem32BitFloat) {
    runFullPipeline("LisbonElevation.tif");
}

TEST_F(EdgeRasterTest, Dem1mResolution) {
    runFullPipeline("dom1_32_356_5699_1_nw_2020.tif");
}

TEST_F(EdgeRasterTest, GlobalPrecipitation) {
    runFullPipeline("gpm_1d.20240617.tif");
}

TEST_F(EdgeRasterTest, RgbWildfires) {
    runFullPipeline("wildfires.tiff");
}

TEST_F(EdgeRasterTest, RgbaCyprus) {
    runFullPipeline("gadas-cyprus.tif");
}

TEST_F(EdgeRasterTest, PastureEurope) {
    runFullPipeline("eu_pasture.tiff");
}

TEST_F(EdgeRasterTest, WindDirection) {
    runFullPipeline("wind_direction.tif");
}

TEST_F(EdgeRasterTest, NoPixelScaleOrTiepoints) {
    runFullPipeline("no_pixelscale_or_tiepoints.tiff");
}

TEST_F(EdgeRasterTest, GlobalWheatHarvest) {
    runFullPipeline("spam2005v3r2_harvested-area_wheat_total.tiff");
}

} // namespace
