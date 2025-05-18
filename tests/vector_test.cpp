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

}
