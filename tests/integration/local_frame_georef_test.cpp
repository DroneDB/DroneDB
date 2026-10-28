/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The georeference of the local frame of a model or a splat: its sidecars (ENU origin, ODM
// coords.txt, projected CRS + offset), its footprint, and the entries that carry it. The WGS84
// values expected of the projected kind were computed independently with PROJ
// (cs2cs -d 9 EPSG:32615 EPSG:4326), not with this code.

#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include "contract_fixtures.h"
#include "ddb.h"
#include "entry.h"
#include "gdal_inc.h"
#include "gtest/gtest.h"
#include "json.h"
#include "json_schema_check.h"
#include "local_frame_georef.h"
#include "mio.h"
#include "test.h"
#include "testarea.h"

namespace
{
    using namespace ddb;

    void writeText(const fs::path &path, const std::string &content)
    {
        fs::create_directories(path.parent_path());
        std::ofstream out(path.string(), std::ios::binary);
        out << content;
    }

    // The ODM project of the brighton dataset: UTM 15N, offset 576705 5188170
    const char *kBrightonCoords = "WGS84 UTM 15N\n576705 5188170\n0.5 0.25 10.0\n";
    constexpr double kBrightonLat = 46.842651237, kBrightonLon = -91.994009513;
    constexpr double kTolerance = 1e-8; // degrees: about a millimetre

    // An antimatter15 .splat of n splats (32 bytes each)
    void writeSplatFile(const fs::path &path, int n)
    {
        std::ofstream out(path.string(), std::ios::binary);
        for (int i = 0; i < n; ++i)
        {
            const float pos[3] = {static_cast<float>(i % 10), static_cast<float>(i / 10), 0.0f};
            const float scale[3] = {0.01f, 0.01f, 0.01f};
            const uint8_t rgba[4] = {128, 130, 132, 255};
            const uint8_t rot[4] = {255, 128, 128, 128};
            out.write(reinterpret_cast<const char *>(pos), sizeof(pos));
            out.write(reinterpret_cast<const char *>(scale), sizeof(scale));
            out.write(reinterpret_cast<const char *>(rgba), sizeof(rgba));
            out.write(reinterpret_cast<const char *>(rot), sizeof(rot));
        }
    }

    void writeObjBox(const fs::path &path)
    {
        writeText(path, "v 0 0 0\nv 10 0 0\nv 0 20 0\nv 0 0 5\nf 1 2 3\nf 1 4 2\n");
    }

    TEST(localFrameGeoref, odmCoordsUtmNorth)
    {
        TestArea ta(TEST_NAME);
        const fs::path model = ta.getPath("model.obj");
        writeObjBox(model);
        writeText(ta.getPath("coords.txt"), kBrightonCoords);

        const auto g = detectLocalFrameGeoref(model.string());
        ASSERT_TRUE(g.has_value());
        EXPECT_EQ(g->kind, LocalFrameGeoref::Kind::Projected);
        EXPECT_EQ(g->crs, "EPSG:32615");
        EXPECT_EQ(g->offset[0], 576705.0);
        EXPECT_EQ(g->offset[1], 5188170.0);
        EXPECT_EQ(g->offset[2], 0.0);

        const Wgs84Position origin = localFrameOrigin(*g);
        EXPECT_NEAR(origin.latitude, kBrightonLat, kTolerance);
        EXPECT_NEAR(origin.longitude, kBrightonLon, kTolerance);
    }

    TEST(localFrameGeoref, odmCoordsUtmSouth)
    {
        TestArea ta(TEST_NAME);
        const fs::path model = ta.getPath("model.obj");
        writeObjBox(model);
        // Lower case and a Windows line ending, as a file written by another tool may have
        writeText(ta.getPath("odm_georeferencing/coords.txt"), "wgs84 utm 56s\r\n334000 6252000\r\n");

        const auto g = detectLocalFrameGeoref(model.string());
        ASSERT_TRUE(g.has_value());
        EXPECT_EQ(g->crs, "EPSG:32756");
        const Wgs84Position origin = localFrameOrigin(*g);
        EXPECT_NEAR(origin.latitude, -33.859261705, kTolerance);
        EXPECT_NEAR(origin.longitude, 151.205514687, kTolerance);
    }

    TEST(localFrameGeoref, odmCoordsWithAProjString)
    {
        TestArea ta(TEST_NAME);
        const fs::path model = ta.getPath("model.obj");
        writeObjBox(model);
        writeText(ta.getPath("coords.txt"), "+proj=utm +zone=15 +datum=WGS84 +units=m +no_defs\n576705 5188170 12.5\n");

        const auto g = detectLocalFrameGeoref(model.string());
        ASSERT_TRUE(g.has_value());
        EXPECT_EQ(g->crs, "+proj=utm +zone=15 +datum=WGS84 +units=m +no_defs");
        EXPECT_EQ(g->offset[2], 12.5);
        const Wgs84Position origin = localFrameOrigin(*g);
        EXPECT_NEAR(origin.latitude, kBrightonLat, kTolerance);
        EXPECT_NEAR(origin.longitude, kBrightonLon, kTolerance);
        EXPECT_NEAR(origin.altitude, 12.5, 1e-6);
    }

    TEST(localFrameGeoref, malformedCoordsGiveNoGeoref)
    {
        const std::vector<std::pair<std::string, std::string>> malformed = {
            {"zone out of range", "WGS84 UTM 61N\n576705 5188170\n"},
            {"no offset line", "WGS84 UTM 15N\n"},
            {"empty file", ""},
            {"offset is not numbers", "WGS84 UTM 15N\neast north\n"},
            {"offset of one number", "WGS84 UTM 15N\n576705\n"},
            {"offset with trailing text", "WGS84 UTM 15N\n576705 5188170 12 13\n"},
            {"offset with garbage after it", "WGS84 UTM 15N\n576705 5188170 x\n"},
            {"unknown CRS", "not a coordinate system\n576705 5188170\n"},
            {"geographic CRS", "EPSG:4326\n576705 5188170\n"},
            {"offset outside the CRS", "WGS84 UTM 15N\n1e300 5188170\n"},
        };
        for (const auto &[what, content] : malformed)
        {
            TestArea ta(TEST_NAME, true);
            const fs::path model = ta.getPath("model.obj");
            writeObjBox(model);
            writeText(ta.getPath("coords.txt"), content);
            EXPECT_FALSE(detectLocalFrameGeoref(model.string()).has_value()) << what;
        }
    }

    TEST(localFrameGeoref, sidecarWithACrsAndAnOffset)
    {
        TestArea ta(TEST_NAME);
        const fs::path model = ta.getPath("model.obj");
        writeObjBox(model);

        writeText(ta.getPath("georef.json"), R"({"crs": "EPSG:32615", "offset": [576705, 5188170]})");
        auto g = detectLocalFrameGeoref(model.string());
        ASSERT_TRUE(g.has_value());
        EXPECT_EQ(g->kind, LocalFrameGeoref::Kind::Projected);
        EXPECT_EQ(g->crs, "EPSG:32615");
        EXPECT_EQ(g->offset[2], 0.0) << "the vertical offset defaults to 0";

        writeText(ta.getPath("georef.json"), R"({"crs": "EPSG:32615", "offset": [576705, 5188170, 30.5]})");
        g = detectLocalFrameGeoref(model.string());
        ASSERT_TRUE(g.has_value());
        EXPECT_EQ(g->offset[2], 30.5);
        EXPECT_NEAR(localFrameOrigin(*g).latitude, kBrightonLat, kTolerance);

        // A WKT works as well as a code
        OGRSpatialReference srs;
        srs.importFromEPSG(32615);
        char *wkt = nullptr;
        srs.exportToWkt(&wkt);
        json j = {{"crs", wkt}, {"offset", {576705, 5188170}}};
        CPLFree(wkt);
        writeText(ta.getPath("georef.json"), j.dump());
        g = detectLocalFrameGeoref(model.string());
        ASSERT_TRUE(g.has_value());
        EXPECT_NEAR(localFrameOrigin(*g).longitude, kBrightonLon, kTolerance);
    }

    TEST(localFrameGeoref, sidecarThatIsNotUsableIsSkipped)
    {
        const std::vector<std::string> unusable = {
            R"({"crs": "EPSG:32615", "offset": [576705]})",
            R"({"crs": "EPSG:32615", "offset": "576705 5188170"})",
            R"({"crs": "EPSG:32615", "offset": [576705, "5188170"]})",
            R"({"crs": "EPSG:4326", "offset": [10, 20]})",
            R"({"crs": "nonsense", "offset": [576705, 5188170]})",
            R"({"crs": "/etc/passwd", "offset": [576705, 5188170]})",
            R"({"crs": "EPSG:32615"})",
            R"({"offset": [576705, 5188170]})",
            R"(not json)",
        };
        for (const auto &content : unusable)
        {
            TestArea ta(TEST_NAME, true);
            const fs::path model = ta.getPath("model.obj");
            writeObjBox(model);
            writeText(ta.getPath("georef.json"), content);
            // The next sidecar of the search is used instead
            writeText(ta.getPath("coords.txt"), kBrightonCoords);

            const auto g = detectLocalFrameGeoref(model.string());
            ASSERT_TRUE(g.has_value()) << content;
            EXPECT_EQ(g->kind, LocalFrameGeoref::Kind::Projected) << content;
            EXPECT_EQ(g->crs, "EPSG:32615") << content;
            EXPECT_EQ(g->offset[0], 576705.0) << content;
        }
    }

    // Each sidecar added below beats all those added before it
    TEST(localFrameGeoref, searchOrder)
    {
        TestArea ta(TEST_NAME);
        const fs::path model = ta.getPath("project/models/mymodel.obj");
        writeObjBox(model);
        const fs::path project = ta.getPath("project");
        const fs::path dir = ta.getPath("project/models");

        const auto coords = [](int x) { return "WGS84 UTM 15N\n" + std::to_string(x) + " 5188170\n"; };
        const auto lla = [](int lat) { return "{\"latitude\": " + std::to_string(lat) + ", \"longitude\": 9}"; };
        const auto xOf = [&]() {
            const auto g = detectLocalFrameGeoref(model.string());
            return g.has_value() && g->kind == LocalFrameGeoref::Kind::Projected ? static_cast<int>(g->offset[0]) : -1;
        };
        const auto latOf = [&]() {
            const auto g = detectLocalFrameGeoref(model.string());
            return g.has_value() && g->kind == LocalFrameGeoref::Kind::Enu ? static_cast<int>(g->origin.latitude) : -1;
        };

        EXPECT_FALSE(detectLocalFrameGeoref(model.string()).has_value());

        writeText(project / "odm_georeferencing" / "coords.txt", coords(100000));
        EXPECT_EQ(xOf(), 100000);
        writeText(project / "opensfm" / "reference_lla.json", lla(11));
        EXPECT_EQ(latOf(), 11);
        writeText(project / "reference_lla.json", lla(12));
        EXPECT_EQ(latOf(), 12);
        writeText(dir / "odm_georeferencing" / "coords.txt", coords(200000));
        EXPECT_EQ(xOf(), 200000);
        writeText(dir / "coords.txt", coords(300000));
        EXPECT_EQ(xOf(), 300000);
        writeText(dir / "reference_lla.json", lla(13));
        EXPECT_EQ(latOf(), 13);
        writeText(dir / "georef.json", R"({"crs": "EPSG:32615", "offset": [400000, 5188170]})");
        EXPECT_EQ(xOf(), 400000);
        writeText(dir / "mymodel.geo.json", lla(14));
        EXPECT_EQ(latOf(), 14);
        // The sidecar of another file of the folder is not this file's
        writeText(dir / "other.geo.json", lla(15));
        EXPECT_EQ(latOf(), 14);
    }

    TEST(localFrameGeoref, propertyRoundTrip)
    {
        const LocalFrameGeoref enu = LocalFrameGeoref::enu(45.5, 9.25, 120.0);
        EXPECT_EQ(json(enu), json::parse(R"({"kind": "enu", "origin": [45.5, 9.25, 120.0]})"));
        const auto enuBack = localFrameGeorefFromProperty(json(enu));
        ASSERT_TRUE(enuBack.has_value());
        EXPECT_EQ(enuBack->kind, LocalFrameGeoref::Kind::Enu);
        EXPECT_EQ(enuBack->origin.latitude, 45.5);
        EXPECT_EQ(enuBack->origin.longitude, 9.25);
        EXPECT_EQ(enuBack->origin.altitude, 120.0);

        const LocalFrameGeoref projected = LocalFrameGeoref::projected("EPSG:32615", 576705, 5188170, 3);
        EXPECT_EQ(json(projected),
                  json::parse(R"({"kind": "projected", "crs": "EPSG:32615", "offset": [576705.0, 5188170.0, 3.0]})"));
        const auto back = localFrameGeorefFromProperty(json(projected));
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(back->kind, LocalFrameGeoref::Kind::Projected);
        EXPECT_EQ(back->crs, "EPSG:32615");
        EXPECT_EQ(back->offset[1], 5188170.0);

        for (const char *bad : {"null", "{}", R"({"kind": "enu"})", R"({"kind": "enu", "origin": [1, 2]})",
                                R"({"kind": "enu", "origin": [100, 2, 3]})",
                                R"({"kind": "projected", "crs": "EPSG:32615"})",
                                R"({"kind": "projected", "crs": "EPSG:32615", "offset": [1, 2]})",
                                R"({"kind": "mercator", "origin": [1, 2, 3]})"})
            EXPECT_FALSE(localFrameGeorefFromProperty(json::parse(bad)).has_value()) << bad;
    }

    // The 4 corners of the extent are converted from the CRS: not an axis-aligned rectangle in WGS84
    TEST(localFrameGeoref, projectedFootprintIsExact)
    {
        const LocalFrameGeoref g = LocalFrameGeoref::projected("EPSG:32615", 576705, 5188170);
        const BasicPolygonGeometry ring = localFrameFootprint(g, LocalBounds{-100, -50, 200, 150});

        // lon, lat of (576605, 5188120), (576905, 5188120), (576905, 5188320), (576605, 5188320)
        const double expected[5][2] = {{-91.995329200, 46.842212874},
                                       {-91.991395337, 46.842178276},
                                       {-91.991361655, 46.843977799},
                                       {-91.995295649, 46.844012399},
                                       {-91.995329200, 46.842212874}};
        ASSERT_EQ(ring.size(), 5);
        for (int i = 0; i < 5; ++i)
        {
            EXPECT_NEAR(ring.points[i].x, expected[i][0], kTolerance) << i;
            EXPECT_NEAR(ring.points[i].y, expected[i][1], kTolerance) << i;
            EXPECT_EQ(ring.points[i].z, 0.0);
        }

        const json geometry = localFrameFootprintGeometry(g, LocalBounds{-100, -50, 200, 150});
        EXPECT_EQ(geometry["type"], "Polygon");
        ASSERT_EQ(geometry["coordinates"].size(), 1u);
        const json &outer = geometry["coordinates"][0];
        ASSERT_EQ(outer.size(), 5u);
        for (int i = 0; i < 5; ++i)
        {
            ASSERT_EQ(outer[i].size(), 2u);
            EXPECT_NEAR(outer[i][0].get<double>(), expected[i][0], kTolerance);
            EXPECT_NEAR(outer[i][1].get<double>(), expected[i][1], kTolerance);
        }
        EXPECT_EQ(outer[0], outer[4]);
    }

    TEST(localFrameGeoref, projectedFootprintInTheSouthernHemisphere)
    {
        const LocalFrameGeoref g = LocalFrameGeoref::projected("EPSG:32756", 334000, 6252000);
        const BasicPolygonGeometry ring = localFrameFootprint(g, LocalBounds{-50, -100, 150, 100});
        ASSERT_EQ(ring.size(), 5);
        // (333950, 6251900) and (334150, 6252100)
        EXPECT_NEAR(ring.points[0].x, 151.204955544, kTolerance);
        EXPECT_NEAR(ring.points[0].y, -33.860155304, kTolerance);
        EXPECT_NEAR(ring.points[2].x, 151.207154359, kTolerance);
        EXPECT_NEAR(ring.points[2].y, -33.858383828, kTolerance);
    }

    // The ENU kind keeps the equirectangular footprint models have always had
    TEST(localFrameGeoref, enuFootprintIsEquirectangular)
    {
        const LocalFrameGeoref g = LocalFrameGeoref::enu(45.0, 9.0, 100.0);
        EXPECT_EQ(localFrameOrigin(g).altitude, 100.0);

        const BasicPolygonGeometry ring = localFrameFootprint(g, LocalBounds{0, 0, 10, 20});
        const double perDegreeLat = 111320.0;
        const double perDegreeLon = 111320.0 * std::cos(45.0 * 0.017453292519943295);
        ASSERT_EQ(ring.size(), 5);
        EXPECT_NEAR(ring.points[0].x, 9.0, 1e-12);
        EXPECT_NEAR(ring.points[0].y, 45.0, 1e-12);
        EXPECT_NEAR(ring.points[1].x, 9.0 + 10.0 / perDegreeLon, 1e-12);
        EXPECT_NEAR(ring.points[2].y, 45.0 + 20.0 / perDegreeLat, 1e-12);
        EXPECT_NEAR(ring.points[3].x, 9.0, 1e-12);
        EXPECT_NEAR(ring.points[4].y, 45.0, 1e-12);
    }

    // The two kinds agree where the approximation is good: a small extent near the origin
    TEST(localFrameGeoref, enuAndProjectedFootprintsAgreeAtSceneScale)
    {
        const LocalFrameGeoref projected = LocalFrameGeoref::projected("EPSG:32615", 576705, 5188170);
        const LocalFrameGeoref enu = LocalFrameGeoref::enu(kBrightonLat, kBrightonLon);
        const LocalBounds bounds{-30, -30, 30, 30};
        const BasicPolygonGeometry a = localFrameFootprint(projected, bounds);
        const BasicPolygonGeometry b = localFrameFootprint(enu, bounds);
        for (int i = 0; i < 5; ++i)
        {
            // 1e-6 degrees is about 0.1 m
            EXPECT_NEAR(a.points[i].x, b.points[i].x, 1e-5) << i;
            EXPECT_NEAR(a.points[i].y, b.points[i].y, 1e-5) << i;
        }
    }

    TEST(localFrameGeoref, modelWithAProjectedGeorefHasAnExactFootprint)
    {
        TestArea ta(TEST_NAME);
        const fs::path model = ta.getPath("model.obj");
        writeObjBox(model);
        writeText(ta.getPath("coords.txt"), kBrightonCoords);

        Entry e;
        parseEntry(model, ta.getFolder(), e, false);

        ASSERT_TRUE(e.properties.contains("georeferenced"));
        EXPECT_TRUE(e.properties["georeferenced"].get<bool>());
        EXPECT_EQ(e.properties["georef"],
                  json::parse(R"({"kind": "projected", "crs": "EPSG:32615", "offset": [576705.0, 5188170.0, 0.0]})"));

        ASSERT_EQ(e.point_geom.size(), 1);
        EXPECT_NEAR(e.point_geom.getPoint(0).x, kBrightonLon, kTolerance);
        EXPECT_NEAR(e.point_geom.getPoint(0).y, kBrightonLat, kTolerance);

        // The box is [0, 10] x [0, 20] around the offset: (576705, 5188170) .. (576715, 5188190)
        ASSERT_EQ(e.polygon_geom.size(), 5);
        const double expected[5][2] = {{-91.994009513, 46.842651237},
                                       {-91.993878384, 46.842650085},
                                       {-91.993875024, 46.842830037},
                                       {-91.994006154, 46.842831190},
                                       {-91.994009513, 46.842651237}};
        for (int i = 0; i < 5; ++i)
        {
            EXPECT_NEAR(e.polygon_geom.getPoint(i).x, expected[i][0], kTolerance) << i;
            EXPECT_NEAR(e.polygon_geom.getPoint(i).y, expected[i][1], kTolerance) << i;
        }
    }

    TEST(localFrameGeoref, modelWithAnEnuGeorefKeepsItsProperties)
    {
        TestArea ta(TEST_NAME);
        const fs::path model = ta.getPath("model.obj");
        writeObjBox(model);
        writeText(ta.getPath("reference_lla.json"), R"({"latitude": 45.0, "longitude": 9.0, "altitude": 100.0})");

        Entry e;
        parseEntry(model, ta.getFolder(), e, false);
        EXPECT_TRUE(e.properties["georeferenced"].get<bool>());
        EXPECT_EQ(e.properties["georef"], json::parse(R"({"kind": "enu", "origin": [45.0, 9.0, 100.0]})"));
        ASSERT_EQ(e.point_geom.size(), 1);
        EXPECT_NEAR(e.point_geom.getPoint(0).z, 100.0, 1e-9);
    }

    // A splat is georeferenced at index time by its origin; its footprint needs the bounds of a
    // build, so the entry carries no polygon
    TEST(localFrameGeoref, splatIndexedWithAProjectedGeoreferenceHasItsOriginAsPoint)
    {
        TestArea ta(TEST_NAME);
        const fs::path splat = ta.getPath("scene.splat");
        writeSplatFile(splat, 20);
        writeText(ta.getPath("georef.json"), R"({"crs": "EPSG:32615", "offset": [576705, 5188170, 0]})");

        Entry e;
        parseEntry(splat, ta.getFolder(), e, false);

        EXPECT_EQ(e.type, EntryType::GaussianSplat);
        EXPECT_EQ(e.properties["splatCount"], 20) << "the splat metadata is kept";
        ASSERT_TRUE(e.properties.contains("georeferenced"));
        EXPECT_TRUE(e.properties["georeferenced"].get<bool>());
        EXPECT_EQ(e.properties["georef"],
                  json::parse(R"({"kind": "projected", "crs": "EPSG:32615", "offset": [576705.0, 5188170.0, 0.0]})"));
        ASSERT_EQ(e.point_geom.size(), 1);
        EXPECT_NEAR(e.point_geom.getPoint(0).x, kBrightonLon, kTolerance);
        EXPECT_NEAR(e.point_geom.getPoint(0).y, kBrightonLat, kTolerance);
        EXPECT_EQ(e.polygon_geom.size(), 0);
    }

    TEST(localFrameGeoref, splatIndexedWithAnEnuGeoreferenceHasItsOriginAsPoint)
    {
        TestArea ta(TEST_NAME);
        const fs::path splat = ta.getPath("scene.splat");
        writeSplatFile(splat, 20);
        writeText(ta.getPath("scene.geo.json"), R"({"lat": -33.86, "lon": 151.21, "alt": 25})");

        Entry e;
        parseEntry(splat, ta.getFolder(), e, false);

        EXPECT_TRUE(e.properties["georeferenced"].get<bool>());
        EXPECT_EQ(e.properties["georef"], json::parse(R"({"kind": "enu", "origin": [-33.86, 151.21, 25.0]})"));
        ASSERT_EQ(e.point_geom.size(), 1);
        EXPECT_NEAR(e.point_geom.getPoint(0).x, 151.21, 1e-9);
        EXPECT_NEAR(e.point_geom.getPoint(0).y, -33.86, 1e-9);
        EXPECT_NEAR(e.point_geom.getPoint(0).z, 25.0, 1e-9);
        EXPECT_EQ(e.polygon_geom.size(), 0);
    }

    // A splat without a sidecar stays as it was: no georeferenced member, no geometry
    TEST(localFrameGeoref, splatWithoutASidecarIsLocal)
    {
        TestArea ta(TEST_NAME);
        const fs::path splat = ta.getPath("scene.splat");
        writeSplatFile(splat, 20);

        Entry e;
        parseEntry(splat, ta.getFolder(), e, false);

        EXPECT_EQ(e.properties["splatCount"], 20);
        EXPECT_FALSE(e.properties.contains("georeferenced"));
        EXPECT_FALSE(e.properties.contains("georef"));
        EXPECT_EQ(e.point_geom.size(), 0);
        EXPECT_EQ(e.polygon_geom.size(), 0);
    }

    // What a host reads (DDBGet) of a georeferenced splat and of a model follows entry.schema.json,
    // and the schema refuses a georef of another shape
    TEST(localFrameGeoref, entriesFollowTheContractSchema)
    {
        TestArea ta(TEST_NAME);
        const fs::path inputs = ta.getFolder("inputs");
        writeSplatFile(inputs / "scene.splat", 20);
        writeText(inputs / "georef.json", R"({"crs": "EPSG:32615", "offset": [576705, 5188170]})");
        writeObjBox(inputs / "model.obj");

        const fs::path root = ta.getFolder("dataset");
        contract::makeDataset(root, {inputs / "scene.splat", inputs / "georef.json", inputs / "model.obj"});

        testutils::JsonSchemaCheck schemas(contract::contractDir() / "schemas");
        for (const char *entry : {"scene.splat", "model.obj"})
        {
            char *out = nullptr;
            ASSERT_EQ(DDBGet(root.string().c_str(), entry, &out), DDBERR_NONE) << DDBGetLastError();
            const json j = contract::takeJson(out);
            ASSERT_TRUE(j["properties"].contains("georef")) << entry;
            EXPECT_EQ(j["properties"]["georef"]["kind"], "projected") << entry;
            EXPECT_TRUE(schemas.validate(j, "entry.schema.json").empty()) << entry;

            for (const char *bad :
                 {R"({"kind": "projected", "crs": "EPSG:32615"})", R"({"kind": "enu", "origin": [1, 2]})",
                  R"({"kind": "utm"})", R"({"crs": "EPSG:32615", "offset": [1, 2, 3]})"})
            {
                json broken = j;
                broken["properties"]["georef"] = json::parse(bad);
                EXPECT_FALSE(schemas.validate(broken, "entry.schema.json").empty()) << bad;
            }
        }
    }
}
