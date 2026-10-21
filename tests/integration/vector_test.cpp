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
#include "gdal_inc.h"
#include "gdal_failure_counter.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

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
            // E1: every GPKG layer produced by buildVector MUST have an
            // explicit SRS. A missing SRS used to silently pass the test
            // because the EPSG check was guarded by `if (hSRS != nullptr)`.
            ASSERT_NE(hSRS, nullptr)
                << "GPKG layer " << i << " is missing a spatial reference; "
                << "buildVector must always reproject to EPSG:4326.";

            EXPECT_TRUE(OSRIsGeographic(hSRS))
                << "GPKG layer " << i << " CRS is not geographic";
            const char *authCode = OSRGetAuthorityCode(hSRS, nullptr);
            const char *authName = OSRGetAuthorityName(hSRS, nullptr);
            ASSERT_NE(authName, nullptr);
            ASSERT_NE(authCode, nullptr);
            EXPECT_STREQ(authName, "EPSG");
            EXPECT_STREQ(authCode, "4326");
        }

        GDALClose(hDS);

        // MVT: verify the directory actually contains a real tile pyramid
        // descriptor (metadata.json). We can't always require at least one
        // .pbf tile because some valid inputs (e.g. KML with 0 placemarks)
        // legitimately produce an empty tile set; the GPKG layer/SRS checks
        // above already guarantee buildVector ran to completion.
        const fs::path metadataJson = mvtDir / "metadata.json";
        ASSERT_TRUE(fs::exists(metadataJson))
            << "MVT output at " << mvtDir.string()
            << " is missing metadata.json (expected GDAL MVT driver layout)";
        EXPECT_GT(fs::file_size(metadataJson), static_cast<std::uintmax_t>(0))
            << "MVT metadata.json is empty";
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

    MANUAL_TEST(testVector, geoJsonIta)
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
    MANUAL_TEST(testVector, bigShapefileMilan)
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

    // Writes one feature per (name, WKT) into a GPKG layer of unknown type,
    // the way CAD drawings arrive (one DXF layer, any geometry type).
    void writeMixedGeometrySource(const fs::path &path,
                                  const std::vector<std::pair<std::string, std::string>> &features)
    {
        GDALDriver *gpkg = GetGDALDriverManager()->GetDriverByName("GPKG");
        ASSERT_NE(gpkg, nullptr);
        std::unique_ptr<GDALDataset> ds(
            gpkg->Create(path.string().c_str(), 0, 0, 0, GDT_Unknown, nullptr));
        ASSERT_NE(ds, nullptr);
        OGRSpatialReference wgs84;
        wgs84.importFromEPSG(4326);
        wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        OGRLayer *layer = ds->CreateLayer("drawing", &wgs84, wkbUnknown, nullptr);
        ASSERT_NE(layer, nullptr);
        OGRFieldDefn nameField("name", OFTString);
        ASSERT_EQ(layer->CreateField(&nameField), OGRERR_NONE);
        for (const auto &[name, wkt] : features)
        {
            OGRGeometry *geom = nullptr;
            ASSERT_EQ(OGRGeometryFactory::createFromWkt(wkt.c_str(), &wgs84, &geom), OGRERR_NONE) << wkt;
            OGRFeature feature(layer->GetLayerDefn());
            feature.SetField("name", name.c_str());
            feature.SetGeometryDirectly(geom);
            ASSERT_EQ(layer->CreateFeature(&feature), OGRERR_NONE) << name;
        }
    }

    // Opens the deepest zoom level of an MVT pyramid written by buildVector.
    std::unique_ptr<GDALDataset> openMaxZoom(const fs::path &mvtDir)
    {
        int maxZoom = -1;
        for (const auto &entry : fs::directory_iterator(mvtDir))
        {
            const auto name = entry.path().filename().string();
            if (entry.is_directory() && !name.empty() &&
                std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isdigit(c); }))
                maxZoom = std::max(maxZoom, std::stoi(name));
        }
        if (maxZoom < 0) return nullptr;
        const char *const drivers[] = {"MVT", nullptr};
        return std::unique_ptr<GDALDataset>(GDALDataset::Open(
            (mvtDir / std::to_string(maxZoom)).string().c_str(), GDAL_OF_VECTOR, drivers, nullptr, nullptr));
    }

    // Every feature reaches the MVT whatever its geometry type. GDAL's MVT
    // writer encodes points, lines and polygons (and splits collections), and
    // GDAL linearizes only geometries that contain real arcs: curve-typed
    // containers with straight parts (a DWG's MULTISURFACE of polygons) and
    // polyhedral types reached the writer as is, failed tile by tile with
    // "Unsupported geometry type" and were missing from the tiles.
    TEST(testVector, mvtKeepsEveryGeometryType)
    {
        TestArea ta(TEST_NAME, true);
        const auto src = ta.getPath("drawing.gpkg");
        writeMixedGeometrySource(src, {
            {"multisurface", "MULTISURFACE Z (((9.180 45.460 0, 9.181 45.460 0, 9.181 45.461 0, 9.180 45.460 0)))"},
            {"curvepolygon", "CURVEPOLYGON ((9.182 45.460, 9.183 45.460, 9.183 45.461, 9.182 45.460))"},
            {"multicurve", "MULTICURVE ((9.184 45.460, 9.185 45.461))"},
            {"collection", "GEOMETRYCOLLECTION (MULTISURFACE (((9.186 45.460, 9.187 45.460, 9.187 45.461, 9.186 45.460))),"
                           " POINT (9.186 45.462))"},
            {"triangle", "TRIANGLE ((9.188 45.460, 9.189 45.460, 9.189 45.461, 9.188 45.460))"},
            {"tin", "TIN (((9.190 45.460, 9.191 45.460, 9.191 45.461, 9.190 45.460)))"},
            {"polyhedral", "POLYHEDRALSURFACE Z (((9.192 45.460 1, 9.193 45.460 1, 9.193 45.461 1, 9.192 45.460 1)))"},
            {"arc", "CURVEPOLYGON (CIRCULARSTRING (9.194 45.460, 9.195 45.461, 9.196 45.460, 9.195 45.459, 9.194 45.460))"},
            {"plain", "POLYGON ((9.197 45.460, 9.198 45.460, 9.198 45.461, 9.197 45.460))"},
        });
        const auto out = ta.getPath("build");

        GdalFailureCounter failures;
        ASSERT_NO_THROW(buildVector(src.string(), out.string()));
        EXPECT_EQ(failures.value(), 0) << "GDAL reported failures while building";

        // name -> geometry types found in the tiles (multi folded into single)
        std::map<std::string, std::set<OGRwkbGeometryType>> seen;
        const auto mvt = openMaxZoom(out / "mvt");
        ASSERT_NE(mvt, nullptr) << "Cannot open the MVT pyramid";
        for (OGRLayer *layer : mvt->GetLayers())
            for (const auto &feature : *layer)
                if (const OGRGeometry *g = feature->GetGeometryRef())
                {
                    auto type = wkbFlatten(g->getGeometryType());
                    if (type == wkbMultiPoint) type = wkbPoint;
                    if (type == wkbMultiLineString) type = wkbLineString;
                    if (type == wkbMultiPolygon) type = wkbPolygon;
                    seen[feature->GetFieldAsString("name")].insert(type);
                }

        using Types = std::set<OGRwkbGeometryType>;
        for (const char *name : {"multisurface", "curvepolygon", "triangle", "tin", "polyhedral", "arc", "plain"})
            EXPECT_EQ(seen[name], (Types{wkbPolygon})) << name;
        EXPECT_EQ(seen["multicurve"], (Types{wkbLineString}));
        EXPECT_EQ(seen["collection"], (Types{wkbPoint, wkbPolygon}));
    }
}
