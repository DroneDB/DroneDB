/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "test.h"
#include "testarea.h"
#include "testfs.h"
#include "vector.h"
#include "mio.h"
#include "ddb.h"
#include "logger.h"
#include "gdal_priv.h"

namespace
{

    using namespace ddb;

    void VerifyVector(const fs::path &vector, int layers = 1)
    {
        // Check file exists
        EXPECT_TRUE(fs::exists(vector));

        // Open file with GDAL
        GDALDatasetH hDS = GDALOpenEx(vector.string().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);

        // Check dataset is not null
        EXPECT_NE(hDS, nullptr);

        // Check layer count
        EXPECT_EQ(GDALDatasetGetLayerCount(hDS), layers);

        // Verify that all layers are in WGS84 (EPSG:4326)
        for (int i = 0; i < GDALDatasetGetLayerCount(hDS); i++)
        {
            OGRLayerH hLayer = GDALDatasetGetLayer(hDS, i);
            EXPECT_NE(hLayer, nullptr);

            OGRSpatialReferenceH hSRS = OGR_L_GetSpatialRef(hLayer);
            if (hSRS != nullptr)
            {
                // Verify CRS is geographic (not projected) and is specifically EPSG:4326
                EXPECT_TRUE(OSRIsGeographic(hSRS)) << "Layer " << i << " CRS is not geographic";

                const char* authCode = OSRGetAuthorityCode(hSRS, nullptr);
                const char* authName = OSRGetAuthorityName(hSRS, nullptr);

                // Both authority name and code must be present
                ASSERT_NE(authName, nullptr) << "Layer " << i << " has no authority name";
                ASSERT_NE(authCode, nullptr) << "Layer " << i << " has no authority code";

                // Verify it's exactly EPSG:4326 (WGS84)
                EXPECT_STREQ(authName, "EPSG") << "Layer " << i << " authority is not EPSG";
                EXPECT_STREQ(authCode, "4326") << "Layer " << i << " is not EPSG:4326";
            }
        }

        // Close
        GDALClose(hDS);
    }

    TEST(testVector, geoJson)
    {
        TestArea ta(TEST_NAME);
        fs::path vector = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/test2.geojson", "test.geojson");
        fs::path output = ta.getPath("test.fgb");

        LOGD << "Building vector " << vector.string() << " to " << output.string();

        buildVector(vector.string(), output.string());

        VerifyVector(output);
    }

    TEST(testVector, geoJsonEiffel)
    {
        TestArea ta(TEST_NAME);
        fs::path vector = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/eiffel.geojson", "eiffel.geojson");
        fs::path output = ta.getPath("eiffel.fgb");

        LOGD << "Building vector " << vector.string() << " to " << output.string();

        buildVector(vector.string(), output.string());

        VerifyVector(output);
    }

    TEST(testVector, geoJsonIta)
    {

        // URL of the test archive
        const auto archiveUrl = "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/ita.zip";

        // Create an instance of TestFS
        TestFS testFS(archiveUrl, "ita", true);

        const std::string vector = "ita.geojson";
        const std::string output = "ita.fgb";

        LOGD << "Building vector " << vector << " to " << output;

        buildVector(vector, output);

        VerifyVector(output);
    }

    TEST(testVector, shapeFileComplete)
    {

        const auto archiveUrl = "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/shapefile/shape_shape_complete.zip";

        TestFS testFS(archiveUrl, "shape_shape_complete", true);

        const auto vector = "shape.shp";
        const auto output = "shape.fgb";

        LOGD << "Building vector " << vector << " to " << output;

        buildVector(vector, output);

        VerifyVector(output);
    }

    TEST(testVector, shapeLineComplete)
    {

        const auto archiveUrl = "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/shapefile/shape_line_complete2.zip";

        TestFS testFS(archiveUrl, "shape_line_complete", true);

        const auto vector = "line.shp";
        const auto output = "line.fgb";

        LOGD << "Building vector " << vector << " to " << output;

        buildVector(vector, output);

        VerifyVector(output);
    }

    TEST(testVector, shapeIta)
    {

        const auto archiveUrl = "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/shapefile/shape_ita.zip";

        TestFS testFS(archiveUrl, "shape_ita", true);

        const auto vector = "ita.shp";
        const auto output = "ita.fgb";

        LOGD << "Building vector " << vector << " to " << output;

        buildVector(vector, output);

        VerifyVector(output);
    }

    TEST(testVector, shapePackLine)
    {

        TestArea ta(TEST_NAME);

        const auto vector = "line-pack.shz";

        fs::path linePack = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/shapefile/line-pack.shz", vector);

        const auto input = ta.getPath(vector);
        const auto output = ta.getPath("line-pack.fgb");

        LOGD << "Building vector " << input << " to " << output;

        buildVector(input.string(), output.string());

        VerifyVector(output);
    }

    TEST(testVector, shapePackPoint)
    {

        TestArea ta(TEST_NAME);

        const auto vector = "point-pack.shz";

        fs::path linePack = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/shapefile/point-pack.shz", vector);

        const auto input = ta.getPath(vector);
        const auto output = ta.getPath("point-pack.fgb");

        LOGD << "Building vector " << input << " to " << output;

        buildVector(input.string(), output.string());

        VerifyVector(output);
    }

    TEST(testVector, shapePackShape)
    {

        TestArea ta(TEST_NAME);

        const auto vector = "shape-pack.shz";

        fs::path linePack = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/shapefile/shape-pack.shz", vector);

        const auto input = ta.getPath(vector);
        const auto output = ta.getPath("shape-pack.fgb");

        LOGD << "Building vector " << input << " to " << output;

        buildVector(input.string(), output.string());

        VerifyVector(output);
    }

    TEST(testVector, dxf1)
    {

        TestArea ta(TEST_NAME);

        const auto vector = "autocad.dxf";

        fs::path linePack = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/autocad/autocad.dxf", vector);

        const auto input = ta.getPath(vector);
        const auto output = ta.getPath("autocad.fgb");

        LOGD << "Building vector " << input << " to " << output;

        buildVector(input.string(), output.string());

        VerifyVector(output);
    }

    TEST(testVector, dxf2)
    {

        TestArea ta(TEST_NAME);

        const auto vector = "civil_war_by_campaign.dxf";

        fs::path linePack = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/autocad/civil_war_by_campaign.dxf", vector);

        const auto input = ta.getPath(vector);
        const auto output = ta.getPath("civil_war_by_campaign.fgb");

        LOGD << "Building vector " << input << " to " << output;

        buildVector(input.string(), output.string());

        VerifyVector(output);
    }

    TEST(testVector, gpkg)
    {

        TestArea ta(TEST_NAME);

        const auto vector = "test.gpkg";

        fs::path linePack = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/test.gpkg", vector);

        const auto input = ta.getPath(vector);
        const auto output = ta.getPath("test.fgb");

        LOGD << "Building vector " << input << " to " << output;

        buildVector(input.string(), output.string());

        VerifyVector(output);
    }

    TEST(testVector, gml)
    {

        TestArea ta(TEST_NAME);

        const auto vector = "test.gml";

        fs::path linePack = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/test.gml", vector);

        const auto input = ta.getPath(vector);
        const auto output = ta.getPath("test.fgb");

        LOGD << "Building vector " << input << " to " << output;

        buildVector(input.string(), output.string());

        VerifyVector(output);
    }

    TEST(testVector, kml1)
    {

        TestArea ta(TEST_NAME);

        const auto vector = "cornishlight.kml";

        fs::path linePack = ta.downloadTestAsset("https://raw.githubusercontent.com/DroneDB/test_data/refs/heads/master/vector/cornishlight.kml", vector);

        const auto input = ta.getPath(vector);
        const auto output = ta.getPath("cornishlight.fgb");

        LOGD << "Building vector " << input << " to " << output;

        buildVector(input.string(), output.string());

        VerifyVector(output);
    }

    TEST(testVector, kml2)
    {

        TestArea ta(TEST_NAME);

        const auto vector = "tour_de_france.kml";

        fs::path linePack = ta.downloadTestAsset("https://raw.githubusercontent.com/DroneDB/test_data/refs/heads/master/vector/tour_de_france.kml", vector);

        const auto input = ta.getPath(vector);
        const auto output = ta.getPath("tour_de_france.fgb");

        LOGD << "Building vector " << input << " to " << output;

        buildVector(input.string(), output.string());

        VerifyVector(output);
    }

    TEST(testVector, verifyKMLDriver)
    {
        // Check if the KML driver is available
        EXPECT_NE(GDALGetDriverByName("LIBKML"), nullptr);

    }

    TEST(testVector, kmz1)
    {

        TestArea ta(TEST_NAME);

        const auto vector = "blackbirds.kmz";

        fs::path linePack = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/blackbirds.kmz", vector);

        const auto input = ta.getPath(vector);
        const auto output = ta.getPath("blackbirds.fgb");

        LOGD << "Building vector " << input << " to " << output;

        buildVector(input.string(), output.string());

        VerifyVector(output);
    }

    TEST(testVector, kmz2)
    {

        TestArea ta(TEST_NAME);

        const auto vector = "civil_war_by_campaign.kmz";

        fs::path linePack = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/civil_war_by_campaign.kmz", vector);

        const auto input = ta.getPath(vector);
        const auto output = ta.getPath("civil_war_by_campaign.fgb");

        LOGD << "Building vector " << input << " to " << output;

        buildVector(input.string(), output.string());

        VerifyVector(output);
    }

    TEST(testVector, kmz3)
    {

        TestArea ta(TEST_NAME);

        const auto vector = "tour_de_france.kmz";

        fs::path linePack = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/tour_de_france.kmz", vector);

        const auto input = ta.getPath(vector);
        const auto output = ta.getPath("tour_de_france.fgb");

        LOGD << "Building vector " << input << " to " << output;

        buildVector(input.string(), output.string());

        VerifyVector(output);
    }

    TEST(testVector, bigShapefileMilan)
    {

        // URL of the test archive
        // This shapefile is in EPSG:6707 (RDN2008 / UTM zone 32N with N-E axis order)
        // It tests that the conversion properly reprojects to WGS84 (EPSG:4326)
        const auto archiveUrl = "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/A010101.zip";

        // Create an instance of TestFS
        TestFS testFS(archiveUrl, "A010101", true);

        const std::string vector = "A010101.shp";
        const std::string output = "A010101.fgb";

        LOGD << "Building vector " << vector << " to " << output;

        buildVector(vector, output);

        VerifyVector(output);

        // Additional verification: check that coordinates are in WGS84 range (degrees, not meters)
        GDALDatasetH hDS = GDALOpenEx(output.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
        ASSERT_NE(hDS, nullptr);

        OGRLayerH hLayer = GDALDatasetGetLayer(hDS, 0);
        ASSERT_NE(hLayer, nullptr);

        // Get the extent - this MUST succeed, otherwise the test should fail
        OGREnvelope extent;
        ASSERT_EQ(OGR_L_GetExtent(hLayer, &extent, TRUE), OGRERR_NONE) << "Failed to get extent from layer";

        // WGS84 coordinates for Milan area should be roughly:
        // Longitude: 8.5 to 10 degrees
        // Latitude: 45 to 46 degrees
        // If coordinates are still in UTM, they would be around 500000, 5000000 (meters)
        LOGD << "Extent: MinX=" << extent.MinX << ", MinY=" << extent.MinY
             << ", MaxX=" << extent.MaxX << ", MaxY=" << extent.MaxY;

        // Verify coordinates are in degrees (WGS84) not meters (UTM)
        EXPECT_GT(extent.MinX, -180.0);
        EXPECT_LT(extent.MaxX, 180.0);
        EXPECT_GT(extent.MinY, -90.0);
        EXPECT_LT(extent.MaxY, 90.0);

        // More specific check for Milan area (lon ~9, lat ~45)
        EXPECT_GT(extent.MinX, 8.0);   // West of Milan
        EXPECT_LT(extent.MaxX, 10.0);  // East of Milan
        EXPECT_GT(extent.MinY, 44.0);  // South of Milan
        EXPECT_LT(extent.MaxY, 47.0);  // North of Milan

        GDALClose(hDS);
    }

}
