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

    // Verify that buildVector produced the expected layout:
    //   <base>/vec/source.gpkg  (GPKG, EPSG:4326, R-tree per layer)
    //   <base>/mvt/{z}/{x}/{y}.pbf + metadata.json (or similar)
    void VerifyVectorBuild(const fs::path &baseDir, int expectedLayerCount = -1)
    {
        const auto gpkg = baseDir / "vec" / "source.gpkg";
        const auto mvtDir = baseDir / "mvt";

        ASSERT_TRUE(fs::exists(gpkg)) << "Missing " << gpkg.string();
        ASSERT_TRUE(fs::exists(mvtDir)) << "Missing " << mvtDir.string();
        ASSERT_TRUE(fs::is_directory(mvtDir));

        // GPKG: every layer must be EPSG:4326
        GDALDatasetH hDS = GDALOpenEx(gpkg.string().c_str(),
                                     GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
        ASSERT_NE(hDS, nullptr) << "Cannot open GPKG sidecar";

        const int layerCount = GDALDatasetGetLayerCount(hDS);
        ASSERT_GT(layerCount, 0) << "GPKG has no layers";
        if (expectedLayerCount > 0)
            EXPECT_EQ(layerCount, expectedLayerCount);

        for (int i = 0; i < layerCount; i++)
        {
            OGRLayerH hLayer = GDALDatasetGetLayer(hDS, i);
            ASSERT_NE(hLayer, nullptr);

            OGRSpatialReferenceH hSRS = OGR_L_GetSpatialRef(hLayer);
            if (hSRS != nullptr)
            {
                EXPECT_TRUE(OSRIsGeographic(hSRS))
                    << "GPKG layer " << i << " CRS is not geographic";
                const char *authCode = OSRGetAuthorityCode(hSRS, nullptr);
                const char *authName = OSRGetAuthorityName(hSRS, nullptr);
                ASSERT_NE(authName, nullptr);
                ASSERT_NE(authCode, nullptr);
                EXPECT_STREQ(authName, "EPSG");
                EXPECT_STREQ(authCode, "4326");
            }
        }

        GDALClose(hDS);

        // MVT: directory must contain at least one zoom-level subdir or metadata.json
        bool hasMvtContent = false;
        for (const auto &p : fs::directory_iterator(mvtDir))
        {
            hasMvtContent = true;
            break;
        }
        EXPECT_TRUE(hasMvtContent) << "MVT output directory is empty";
    }

    TEST(testVector, geoJson)
    {
        TestArea ta(TEST_NAME);
        fs::path vector = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/test2.geojson",
            "test.geojson");
        fs::path baseDir = ta.getPath("build");

        buildVector(vector.string(), baseDir.string());
        VerifyVectorBuild(baseDir);
    }

    TEST(testVector, geoJsonEiffel)
    {
        TestArea ta(TEST_NAME);
        fs::path vector = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/eiffel.geojson",
            "eiffel.geojson");
        fs::path baseDir = ta.getPath("build");

        buildVector(vector.string(), baseDir.string());
        VerifyVectorBuild(baseDir);
    }

    TEST(testVector, geoJsonIta)
    {
        TestFS testFS(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/ita.zip",
            "ita", true);

        const std::string vector = "ita.geojson";
        const std::string baseDir = "build";

        buildVector(vector, baseDir);
        VerifyVectorBuild(baseDir);
    }

    TEST(testVector, shapeFileComplete)
    {
        TestFS testFS(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/shapefile/shape_shape_complete.zip",
            "shape_shape_complete", true);

        buildVector("shape.shp", "build");
        VerifyVectorBuild("build");
    }

    TEST(testVector, shapeLineComplete)
    {
        TestFS testFS(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/shapefile/shape_line_complete2.zip",
            "shape_line_complete", true);

        buildVector("line.shp", "build");
        VerifyVectorBuild("build");
    }

    TEST(testVector, shapeIta)
    {
        TestFS testFS(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/shapefile/shape_ita.zip",
            "shape_ita", true);

        buildVector("ita.shp", "build");
        VerifyVectorBuild("build");
    }

    TEST(testVector, shapePackLine)
    {
        TestArea ta(TEST_NAME);
        fs::path linePack = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/shapefile/line-pack.shz",
            "line-pack.shz");
        const auto input = ta.getPath("line-pack.shz");
        const auto baseDir = ta.getPath("build");

        buildVector(input.string(), baseDir.string());
        VerifyVectorBuild(baseDir);
    }

    TEST(testVector, shapePackPoint)
    {
        TestArea ta(TEST_NAME);
        ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/shapefile/point-pack.shz",
            "point-pack.shz");
        buildVector(ta.getPath("point-pack.shz").string(),
                    ta.getPath("build").string());
        VerifyVectorBuild(ta.getPath("build"));
    }

    TEST(testVector, shapePackShape)
    {
        TestArea ta(TEST_NAME);
        ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/shapefile/shape-pack.shz",
            "shape-pack.shz");
        buildVector(ta.getPath("shape-pack.shz").string(),
                    ta.getPath("build").string());
        VerifyVectorBuild(ta.getPath("build"));
    }

    TEST(testVector, dxf1)
    {
        TestArea ta(TEST_NAME);
        ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/autocad/autocad.dxf",
            "autocad.dxf");
        buildVector(ta.getPath("autocad.dxf").string(),
                    ta.getPath("build").string());
        VerifyVectorBuild(ta.getPath("build"));
    }

    TEST(testVector, dxf2)
    {
        TestArea ta(TEST_NAME);
        ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/autocad/civil_war_by_campaign.dxf",
            "civil_war_by_campaign.dxf");
        buildVector(ta.getPath("civil_war_by_campaign.dxf").string(),
                    ta.getPath("build").string());
        VerifyVectorBuild(ta.getPath("build"));
    }

    TEST(testVector, gpkgSource)
    {
        TestArea ta(TEST_NAME);
        ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/test.gpkg",
            "test.gpkg");
        buildVector(ta.getPath("test.gpkg").string(),
                    ta.getPath("build").string());
        VerifyVectorBuild(ta.getPath("build"));
    }

    TEST(testVector, gml)
    {
        TestArea ta(TEST_NAME);
        ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/test.gml",
            "test.gml");
        buildVector(ta.getPath("test.gml").string(),
                    ta.getPath("build").string());
        VerifyVectorBuild(ta.getPath("build"));
    }

    TEST(testVector, kml1)
    {
        TestArea ta(TEST_NAME);
        ta.downloadTestAsset(
            "https://raw.githubusercontent.com/DroneDB/test_data/refs/heads/master/vector/cornishlight.kml",
            "cornishlight.kml");
        buildVector(ta.getPath("cornishlight.kml").string(),
                    ta.getPath("build").string());
        VerifyVectorBuild(ta.getPath("build"));
    }

    TEST(testVector, kml2)
    {
        TestArea ta(TEST_NAME);
        ta.downloadTestAsset(
            "https://raw.githubusercontent.com/DroneDB/test_data/refs/heads/master/vector/tour_de_france.kml",
            "tour_de_france.kml");
        buildVector(ta.getPath("tour_de_france.kml").string(),
                    ta.getPath("build").string());
        VerifyVectorBuild(ta.getPath("build"));
    }

    TEST(testVector, verifyKMLDriver)
    {
        EXPECT_NE(GDALGetDriverByName("LIBKML"), nullptr);
    }

    TEST(testVector, verifyMVTDriver)
    {
        // Required for the rewritten buildVector pipeline
        EXPECT_NE(GDALGetDriverByName("MVT"), nullptr)
            << "GDAL build is missing the MVT writer driver";
    }

    TEST(testVector, kmz1)
    {
        TestArea ta(TEST_NAME);
        ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/blackbirds.kmz",
            "blackbirds.kmz");
        buildVector(ta.getPath("blackbirds.kmz").string(),
                    ta.getPath("build").string());
        VerifyVectorBuild(ta.getPath("build"));
    }

    TEST(testVector, kmz2)
    {
        TestArea ta(TEST_NAME);
        ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/civil_war_by_campaign.kmz",
            "civil_war_by_campaign.kmz");
        buildVector(ta.getPath("civil_war_by_campaign.kmz").string(),
                    ta.getPath("build").string());
        VerifyVectorBuild(ta.getPath("build"));
    }

    TEST(testVector, kmz3)
    {
        TestArea ta(TEST_NAME);
        ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/tour_de_france.kmz",
            "tour_de_france.kmz");
        buildVector(ta.getPath("tour_de_france.kmz").string(),
                    ta.getPath("build").string());
        VerifyVectorBuild(ta.getPath("build"));
    }

    /**
     * Verify that conversion from a projected CRS (EPSG:6707 RDN2008/UTM32N N-E
     * axis order) correctly reprojects to WGS84 in the GPKG sidecar.
     */
    TEST(testVector, bigShapefileMilan)
    {
        TestFS testFS(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/A010101.zip",
            "A010101", true);

        buildVector("A010101.shp", "build");
        VerifyVectorBuild("build");

        // Open GPKG and verify coords are in degrees (WGS84) not meters (UTM).
        GDALDatasetH hDS = GDALOpenEx("build/vec/source.gpkg",
                                     GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
        ASSERT_NE(hDS, nullptr);
        OGRLayerH hLayer = GDALDatasetGetLayer(hDS, 0);
        ASSERT_NE(hLayer, nullptr);

        OGREnvelope extent;
        ASSERT_EQ(OGR_L_GetExtent(hLayer, &extent, TRUE), OGRERR_NONE);

        EXPECT_GT(extent.MinX, 8.0);
        EXPECT_LT(extent.MaxX, 10.0);
        EXPECT_GT(extent.MinY, 44.0);
        EXPECT_LT(extent.MaxY, 47.0);

        GDALClose(hDS);
    }

    /**
     * Verify that the GPKG sidecar has an R-tree spatial index per layer
     * (required for efficient WFS / OGC API Features BBOX queries).
     */
    TEST(testVector, spatialIndexVerification)
    {
        TestArea ta(TEST_NAME);
        ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/test2.geojson",
            "test.geojson");
        const auto baseDir = ta.getPath("build");
        buildVector(ta.getPath("test.geojson").string(), baseDir.string());
        VerifyVectorBuild(baseDir);

        const auto gpkg = (baseDir / "vec" / "source.gpkg").string();
        GDALDatasetH hDS = GDALOpenEx(gpkg.c_str(), GDAL_OF_VECTOR,
                                     nullptr, nullptr, nullptr);
        ASSERT_NE(hDS, nullptr);
        OGRLayerH hLayer = GDALDatasetGetLayer(hDS, 0);
        ASSERT_NE(hLayer, nullptr);

        EXPECT_TRUE(OGR_L_TestCapability(hLayer, OLCFastSpatialFilter))
            << "GPKG layer should have R-tree spatial index";

        GDALClose(hDS);
    }

    /**
     * Atomic write: a second call without overwrite must be a no-op
     * (both vec/ and mvt/ already present).
     */
    TEST(testVector, atomicSkipOnRebuild)
    {
        TestArea ta(TEST_NAME);
        ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/test2.geojson",
            "test.geojson");
        const auto baseDir = ta.getPath("build");

        buildVector(ta.getPath("test.geojson").string(), baseDir.string());
        VerifyVectorBuild(baseDir);

        // Capture modification time of one published artifact
        const auto gpkg = baseDir / "vec" / "source.gpkg";
        const auto firstMtime = fs::last_write_time(gpkg);

        // Second call without overwrite: should skip and not rewrite the file.
        buildVector(ta.getPath("test.geojson").string(), baseDir.string());

        const auto secondMtime = fs::last_write_time(gpkg);
        EXPECT_EQ(firstMtime, secondMtime) << "GPKG was rewritten on no-op call";
    }
}
