/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The search of the images that see a place, on an index of images made for the test: nadir cameras of
// known footprint (150 x 100 m at 100 m over the ground), so that the scores, the overlaps and the
// pixels are known.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "camera_model.h"
#include "coverage.h"
#include "entry.h"
#include "exceptions.h"
#include "geo.h"
#include "gtest/gtest.h"
#include "json.h"
#include "test.h"
#include "transaction.h"
#include "utils.h"

namespace
{
    using namespace ddb;

    // The origin of the metres east and north of a test
    struct Site
    {
        double latitude;
        double longitude;
    };
    constexpr Site Origin{45.0, 9.0};
    constexpr double MetresPerDegree = 111195.0;
    constexpr int Width = 6000;
    constexpr int Height = 4000;
    // The footprint of the camera of the test at 100 m, nadir: 150 m wide and 100 m tall
    constexpr double FootprintWidth = 150.0;
    constexpr double FootprintHeight = 100.0;
    const double FootprintDiagonal = std::hypot(FootprintWidth, FootprintHeight);

    // Longitude and latitude of a place east and north of the origin of the test, in metres
    Point place(double east, double north, const Site &site = Origin)
    {
        return Point(site.longitude + east / (MetresPerDegree * std::cos(deg2rad(site.latitude))),
                     site.latitude + north / MetresPerDegree);
    }

    struct Camera
    {
        double east = 0.0;
        double north = 0.0;
        double altitude = 250.0;
        double pitch = -90.0;
        double yaw = 0.0;
        double roll = 0.0;
        double relAltitude = 100.0;
        bool hasOrientation = true;
        Site site = Origin;
    };

    Entry imageOf(const std::string &path, const Camera &camera)
    {
        Entry entry;
        entry.path = path;
        entry.type = EntryType::GeoImage;
        entry.properties = json{{"width", Width},
                                {"height", Height},
                                {"sensorWidth", 13.2},
                                {"sensorHeight", 8.8},
                                {"focalLength", 8.8},
                                {"cameraYaw", camera.yaw},
                                {"cameraPitch", camera.pitch},
                                {"cameraRoll", camera.roll},
                                {"hasCameraOrientation", camera.hasOrientation}};
        const Point at = place(camera.east, camera.north, camera.site);
        entry.point_geom.addPoint(at.x, at.y, camera.altitude);
        calculateFootprint(SensorSize(13.2, 8.8), GeoLocation(at.y, at.x, camera.altitude), Focal(8.8, 24.0),
                           CameraOrientation(camera.pitch, camera.yaw, camera.roll), camera.relAltitude, entry.polygon_geom);
        return entry;
    }

    // A row of the index, as the add writes it
    void insert(Database *db, const Entry &entry)
    {
        auto q = db->query("INSERT INTO entries (path, hash, type, properties, mtime, size, depth, point_geom, polygon_geom) "
                           "VALUES (?, ?, ?, ?, 0, 1, ?, GeomFromText(?, 4326), GeomFromText(?, 4326))");
        q->bind(1, entry.path);
        q->bind(2, entry.path);
        q->bind(3, entryTypeValue(entry.type));
        q->bind(4, entry.properties.dump());
        q->bind(5, static_cast<int>(std::count(entry.path.begin(), entry.path.end(), '/')));
        q->bind(6, entry.point_geom.toWkt());
        q->bind(7, entry.polygon_geom.toWkt());
        q->execute();
    }

    void insertImage(Database *db, const std::string &path, const Camera &camera)
    {
        insert(db, imageOf(path, camera));
    }

    CoverageGeometry pointAt(double east, double north)
    {
        CoverageGeometry geometry;
        geometry.kind = CoverageGeometry::Kind::Point;
        geometry.parts.push_back({place(east, north)});
        return geometry;
    }

    // A rectangle of the ground, by its corners in metres east and north
    CoverageGeometry rectangle(double west, double south, double east, double north, const Site &site = Origin)
    {
        CoverageGeometry geometry;
        geometry.kind = CoverageGeometry::Kind::Polygon;
        geometry.parts.push_back({place(west, south, site), place(east, south, site), place(east, north, site),
                                  place(west, north, site), place(west, south, site)});
        return geometry;
    }

    std::vector<std::string> pathsOf(const std::vector<CoverageMatch> &matches)
    {
        std::vector<std::string> paths;
        for (const auto &match : matches)
            paths.push_back(match.path);
        return paths;
    }

    // ------------------------------------------------------------------ ranking

    TEST(Coverage, RanksByDistanceOfTheMiddleOverTheDiagonal)
    {
        IndexFixture index(TEST_NAME);
        insertImage(index.db.get(), "near.jpg", {.east = 30.0});
        insertImage(index.db.get(), "middle.jpg", {.east = 0.0});
        insertImage(index.db.get(), "edge.jpg", {.east = -60.0});
        insertImage(index.db.get(), "far.jpg", {.east = 5000.0});

        const auto matches = findCoverage(index.db.get(), pointAt(0.0, 0.0), {});
        ASSERT_EQ(pathsOf(matches), (std::vector<std::string>{"middle.jpg", "near.jpg", "edge.jpg"}));
        EXPECT_NEAR(matches[0].score, 0.0, 2e-3);
        EXPECT_NEAR(matches[1].score, 30.0 / FootprintDiagonal, 2e-3);
        EXPECT_NEAR(matches[2].score, 60.0 / FootprintDiagonal, 2e-3);
        EXPECT_LT(matches[0].score, matches[1].score);
        EXPECT_LT(matches[1].score, matches[2].score);
    }

    TEST(Coverage, TiesAreBrokenByPath)
    {
        IndexFixture index(TEST_NAME);
        for (const char *path : {"b.jpg", "c.jpg", "a.jpg", "sub/a.jpg"})
            insertImage(index.db.get(), path, {});

        const auto matches = findCoverage(index.db.get(), pointAt(10.0, 0.0), {});
        EXPECT_EQ(pathsOf(matches), (std::vector<std::string>{"a.jpg", "b.jpg", "c.jpg", "sub/a.jpg"}));
    }

    TEST(Coverage, LimitKeepsTheBest)
    {
        IndexFixture index(TEST_NAME);
        for (int i = 0; i < 10; i++)
            insertImage(index.db.get(), "image" + std::to_string(i) + ".jpg", {.east = i * 5.0});

        CoverageOptions options;
        options.limit = 3;
        const auto matches = findCoverage(index.db.get(), pointAt(0.0, 0.0), options);
        EXPECT_EQ(pathsOf(matches), (std::vector<std::string>{"image0.jpg", "image1.jpg", "image2.jpg"}));

        options.limit = CoverageOptions::MaxLimit;
        EXPECT_EQ(findCoverage(index.db.get(), pointAt(0.0, 0.0), options).size(), 10u);
        // The default
        EXPECT_EQ(findCoverage(index.db.get(), pointAt(0.0, 0.0), {}).size(), 10u);
    }

    TEST(Coverage, DefaultLimitIsTwenty)
    {
        IndexFixture index(TEST_NAME);
        for (int i = 0; i < 25; i++)
            insertImage(index.db.get(), "image" + std::to_string(100 + i) + ".jpg", {.east = i * 1.0});
        EXPECT_EQ(findCoverage(index.db.get(), pointAt(0.0, 0.0), {}).size(), 20u);
    }

    TEST(Coverage, NothingThatIntersectsIsEmpty)
    {
        IndexFixture index(TEST_NAME);
        insertImage(index.db.get(), "a.jpg", {});
        EXPECT_TRUE(findCoverage(index.db.get(), pointAt(1000.0, 1000.0), {}).empty());
        EXPECT_EQ(coverageToJson({}).dump(), "[]");
        // An index with no images
        IndexFixture empty(TEST_NAME + "-empty");
        EXPECT_TRUE(findCoverage(empty.db.get(), pointAt(0.0, 0.0), {}).empty());
    }

    TEST(Coverage, OnlyImagesWithAFootprintAreSearched)
    {
        IndexFixture index(TEST_NAME);
        insertImage(index.db.get(), "photo.jpg", {});

        // A raster and a model with a polygon are not images
        Entry raster = imageOf("ortho.tif", {});
        raster.type = EntryType::GeoRaster;
        insert(index.db.get(), raster);
        Entry model = imageOf("model.obj", {});
        model.type = EntryType::Model;
        insert(index.db.get(), model);

        // An image with no footprint (no GPS, or no camera) has nothing to intersect
        Entry unplaced = imageOf("nofootprint.jpg", {});
        unplaced.polygon_geom.clear();
        insert(index.db.get(), unplaced);
        Entry plain = imageOf("noexif.jpg", {});
        plain.type = EntryType::Image;
        plain.point_geom.clear();
        plain.polygon_geom.clear();
        insert(index.db.get(), plain);

        EXPECT_EQ(pathsOf(findCoverage(index.db.get(), pointAt(0.0, 0.0), {})), (std::vector<std::string>{"photo.jpg"}));
    }

    // ------------------------------------------------------------------ overlap

    TEST(Coverage, OverlapOfAPolygon)
    {
        IndexFixture index(TEST_NAME);
        insertImage(index.db.get(), "a.jpg", {});

        // Inside the footprint, the whole of it
        auto matches = findCoverage(index.db.get(), rectangle(-20, -20, 20, 20), {});
        ASSERT_EQ(matches.size(), 1u);
        EXPECT_NEAR(matches[0].overlap, 1.0, 1e-6);

        // Half of the polygon is outside, across the east edge (at 75 m)
        matches = findCoverage(index.db.get(), rectangle(25, -20, 125, 20), {});
        ASSERT_EQ(matches.size(), 1u);
        EXPECT_NEAR(matches[0].overlap, 0.5, 0.01);

        // The polygon is larger than the footprint: the footprint (150 x 100) over the polygon (300 x 200)
        matches = findCoverage(index.db.get(), rectangle(-150, -100, 150, 100), {});
        ASSERT_EQ(matches.size(), 1u);
        EXPECT_NEAR(matches[0].overlap, 0.25, 0.01);
    }

    TEST(Coverage, OverlapOfAPolygonWithAHole)
    {
        IndexFixture index(TEST_NAME);
        insertImage(index.db.get(), "a.jpg", {});

        // A square of 80 m with a hole of 40 m, inside the footprint
        auto geometry = rectangle(-40, -40, 40, 40);
        const auto hole = rectangle(-20, -20, 20, 20).parts.front();
        geometry.parts.push_back(hole);
        const auto matches = findCoverage(index.db.get(), geometry, {});
        ASSERT_EQ(matches.size(), 1u);
        EXPECT_NEAR(matches[0].overlap, 1.0, 1e-6);
    }

    TEST(Coverage, OverlapOfAPointAndALineIsOne)
    {
        IndexFixture index(TEST_NAME);
        insertImage(index.db.get(), "a.jpg", {});

        EXPECT_EQ(findCoverage(index.db.get(), pointAt(70.0, 0.0), {}).at(0).overlap, 1.0);

        // A line that leaves the footprint
        CoverageGeometry line;
        line.kind = CoverageGeometry::Kind::Line;
        line.parts.push_back({place(0, 0), place(500, 0)});
        const auto matches = findCoverage(index.db.get(), line, {});
        ASSERT_EQ(matches.size(), 1u);
        EXPECT_EQ(matches[0].overlap, 1.0);
        // Its centroid is the middle of the line, 250 m from the middle of the image
        EXPECT_NEAR(matches[0].score, 250.0 / FootprintDiagonal, 5e-3);
    }

    TEST(Coverage, ScoreIsTheDistanceOfCentroidsForAPolygon)
    {
        IndexFixture index(TEST_NAME);
        insertImage(index.db.get(), "a.jpg", {});
        // A rectangle whose centroid is 40 m east of the middle of the image
        const auto matches = findCoverage(index.db.get(), rectangle(20, -10, 60, 10), {});
        ASSERT_EQ(matches.size(), 1u);
        EXPECT_NEAR(matches[0].score, 40.0 / FootprintDiagonal, 2e-3);
    }

    // ------------------------------------------------------------------ pixel

    void expectPixel(const CoverageMatch &match, double x, double y, double tolerance = 20.0)
    {
        ASSERT_TRUE(match.pixel.has_value()) << match.path;
        EXPECT_NEAR(match.pixel->first, x, tolerance) << match.path;
        EXPECT_NEAR(match.pixel->second, y, tolerance) << match.path;
    }

    TEST(Coverage, PixelOfThePointInTheImage)
    {
        IndexFixture index(TEST_NAME);
        insertImage(index.db.get(), "a.jpg", {});

        // The middle, then a quarter of the width to the east, then a quarter of the height to the north
        expectPixel(findCoverage(index.db.get(), pointAt(0.0, 0.0), {}).at(0), Width / 2.0, Height / 2.0);
        expectPixel(findCoverage(index.db.get(), pointAt(37.5, 0.0), {}).at(0), 0.75 * Width, Height / 2.0);
        expectPixel(findCoverage(index.db.get(), pointAt(0.0, 25.0), {}).at(0), Width / 2.0, 0.25 * Height);
    }

    TEST(Coverage, PixelFollowsTheYaw)
    {
        IndexFixture index(TEST_NAME);
        // The camera looks east: the east is the top of the image, and the north its left
        insertImage(index.db.get(), "a.jpg", {.yaw = 90.0});
        expectPixel(findCoverage(index.db.get(), pointAt(25.0, 0.0), {}).at(0), Width / 2.0, 0.25 * Height);
        expectPixel(findCoverage(index.db.get(), pointAt(0.0, 37.5), {}).at(0), 0.25 * Width, Height / 2.0);
    }

    TEST(Coverage, PixelWithNoGpsAltitude)
    {
        IndexFixture index(TEST_NAME);
        // A GPS altitude of 0 puts the ground at the height above ground itself
        insertImage(index.db.get(), "a.jpg", {.altitude = 0.0});
        expectPixel(findCoverage(index.db.get(), pointAt(37.5, 0.0), {}).at(0), 0.75 * Width, Height / 2.0);
    }

    TEST(Coverage, PixelOfATiltedCamera)
    {
        IndexFixture index(TEST_NAME);
        // 60 degrees below the horizon: the axis meets the ground 100 * tan(30 deg) north of the point under it
        insertImage(index.db.get(), "a.jpg", {.pitch = -60.0});
        expectPixel(findCoverage(index.db.get(), pointAt(0.0, 100.0 * std::tan(deg2rad(30.0))), {}).at(0), Width / 2.0,
                    Height / 2.0, 40.0);
    }

    TEST(Coverage, NoPixelWhenAParameterIsMissing)
    {
        IndexFixture index(TEST_NAME);
        insertImage(index.db.get(), "oriented.jpg", {});
        insertImage(index.db.get(), "guessed.jpg", {.hasOrientation = false});

        Entry noSensor = imageOf("nosensor.jpg", {});
        noSensor.properties.erase("sensorWidth");
        insert(index.db.get(), noSensor);
        Entry noSize = imageOf("nosize.jpg", {});
        noSize.properties.erase("width");
        insert(index.db.get(), noSize);
        Entry noPoint = imageOf("nopoint.jpg", {});
        noPoint.point_geom.clear();
        insert(index.db.get(), noPoint);

        const auto matches = findCoverage(index.db.get(), pointAt(0.0, 0.0), {});
        ASSERT_EQ(matches.size(), 5u);
        for (const auto &match : matches)
        {
            // They still rank: the footprint is there
            EXPECT_EQ(match.pixel.has_value(), match.path == "oriented.jpg") << match.path;
        }
    }

    TEST(Coverage, NoPixelWhenThePointIsOutsideTheImage)
    {
        IndexFixture index(TEST_NAME);
        // The footprint is a rectangle of the ground that the camera sees; a point just beyond its east edge is
        // outside the footprint and not in the search, and a line that touches the edge has its centroid beyond
        insertImage(index.db.get(), "a.jpg", {});
        CoverageGeometry line;
        line.kind = CoverageGeometry::Kind::Line;
        line.parts.push_back({place(60, 0), place(400, 0)});
        const auto matches = findCoverage(index.db.get(), line, {});
        ASSERT_EQ(matches.size(), 1u);
        EXPECT_FALSE(matches[0].pixel.has_value());
    }

    TEST(Coverage, JsonShape)
    {
        IndexFixture index(TEST_NAME);
        insertImage(index.db.get(), "a.jpg", {});
        insertImage(index.db.get(), "guessed.jpg", {.east = 20.0, .hasOrientation = false});
        const auto j = coverageToJson(findCoverage(index.db.get(), pointAt(0.0, 0.0), {}));
        ASSERT_EQ(j.size(), 2u);
        EXPECT_EQ(j[0]["path"], "a.jpg");
        EXPECT_TRUE(j[0]["score"].is_number());
        EXPECT_EQ(j[0]["overlap"], 1.0);
        ASSERT_TRUE(j[0]["pixel"].is_array());
        EXPECT_EQ(j[0]["pixel"].size(), 2u);
        EXPECT_TRUE(j[0]["pixel"][0].is_number_integer());
        EXPECT_EQ(j[1]["path"], "guessed.jpg");
        EXPECT_TRUE(j[1]["pixel"].is_null());
        EXPECT_EQ(j[0].size(), 4u);
    }

    // A square of the UTM grid the footprint is made in (a footprint is a rectangle of that grid around the point
    // under the camera), `east` and `north` metres from the point under the camera of a site
    CoverageGeometry utmSquare(const Site &site, double east, double north, double side)
    {
        const UtmProjector projector(getUTMZone(site.latitude, site.longitude));
        const Projected2D under = projector.toUTM(site.latitude, site.longitude);
        const auto corner = [&](double dx, double dy)
        {
            const Geographic2D g = projector.fromUTM(under.x + east + dx * side / 2, under.y + north + dy * side / 2);
            return Point(g.longitude, g.latitude);
        };
        CoverageGeometry geometry;
        geometry.kind = CoverageGeometry::Kind::Polygon;
        geometry.parts.push_back({corner(-1, -1), corner(1, -1), corner(1, 1), corner(-1, 1), corner(-1, -1)});
        return geometry;
    }

    // A polygon of a metre or less, far from the equator and the false easting: its centroid is the one of its
    // corners, not the noise of products of UTM coordinates (millions of metres) that cancel each other
    TEST(Coverage, CentroidOfASmallPolygon)
    {
        // The north (zone 15N, about 5.19e6 m north) and the south (zone 56S, about 6.25e6 m north, a false northing)
        for (const Site site : {Site{46.84, -91.99456}, Site{-33.8688, 151.2093}})
        {
            IndexFixture index(TEST_NAME + (site.latitude > 0 ? "-north" : "-south"));
            insertImage(index.db.get(), "a.jpg", {.site = site});

            for (const double side : {1.0, 0.5})
            {
                SCOPED_TRACE(std::to_string(site.latitude) + ", " + std::to_string(side) + " m");
                // Around the point under the nadir camera: the middle of the footprint and of the image. 1e-6 of
                // the diagonal is 0.2 mm, and a pixel is 25 mm
                auto matches = findCoverage(index.db.get(), utmSquare(site, 0, 0, side), {});
                ASSERT_EQ(matches.size(), 1u);
                EXPECT_NEAR(matches[0].score, 0.0, 1e-6);
                EXPECT_NEAR(matches[0].overlap, 1.0, 1e-6);
                expectPixel(matches[0], Width / 2.0, Height / 2.0, 1.0);

                // 30 m east and 20 m south of it: a fifth of the width to the right and a fifth of the height down
                matches = findCoverage(index.db.get(), utmSquare(site, 30, -20, side), {});
                ASSERT_EQ(matches.size(), 1u);
                EXPECT_NEAR(matches[0].score, std::hypot(30.0, 20.0) / FootprintDiagonal, 1e-6);
                expectPixel(matches[0], 0.7 * Width, 0.7 * Height, 1.0);
            }
        }
    }

    // ------------------------------------------------------------------ arguments

    TEST(Coverage, GeometryFromGeoJson)
    {
        const auto point = parseCoverageGeometry(json::parse(R"({"type":"Point","coordinates":[9.5,45.25,120]})"));
        EXPECT_EQ(point.kind, CoverageGeometry::Kind::Point);
        ASSERT_EQ(point.parts.size(), 1u);
        EXPECT_EQ(point.parts[0][0].x, 9.5);
        EXPECT_EQ(point.parts[0][0].y, 45.25);

        const auto line = parseCoverageGeometry(json::parse(R"({"type":"LineString","coordinates":[[9,45],[9.1,45.1]]})"));
        EXPECT_EQ(line.kind, CoverageGeometry::Kind::Line);
        EXPECT_EQ(line.parts[0].size(), 2u);

        const auto polygon = parseCoverageGeometry(
            json::parse(R"({"type":"Polygon","coordinates":[[[9,45],[9.1,45],[9.1,45.1],[9,45.1],[9,45]],[[9.02,45.02],[9.04,45.02],[9.04,45.04],[9.02,45.02]]]})"));
        EXPECT_EQ(polygon.kind, CoverageGeometry::Kind::Polygon);
        ASSERT_EQ(polygon.parts.size(), 2u);
        EXPECT_EQ(polygon.parts[0].size(), 5u);

        // A ring that is not closed is closed
        const auto open = parseCoverageGeometry(json::parse(R"({"type":"Polygon","coordinates":[[[9,45],[9.1,45],[9.1,45.1]]]})"));
        ASSERT_EQ(open.parts[0].size(), 4u);
        EXPECT_EQ(open.parts[0].front().x, open.parts[0].back().x);
        EXPECT_EQ(open.parts[0].front().y, open.parts[0].back().y);
        EXPECT_EQ(open.toWkt().rfind("POLYGON((", 0), 0u);
    }

    TEST(Coverage, GeometryThatIsRefused)
    {
        for (const char *text : {
                 R"([1,2])",
                 R"("Point")",
                 R"({})",
                 R"({"type":"Point"})",
                 R"({"type":"Point","coordinates":[9]})",
                 R"({"type":"Point","coordinates":"9,45"})",
                 R"({"type":"Point","coordinates":["9","45"]})",
                 R"({"type":"Point","coordinates":[181,45]})",
                 R"({"type":"Point","coordinates":[9,-90.5]})",
                 R"({"type":"MultiPoint","coordinates":[[9,45]]})",
                 R"({"type":"GeometryCollection","geometries":[]})",
                 R"({"type":"Feature","geometry":{"type":"Point","coordinates":[9,45]}})",
                 R"({"type":"LineString","coordinates":[[9,45]]})",
                 R"({"type":"Polygon","coordinates":[]})",
                 R"({"type":"Polygon","coordinates":[[[9,45],[9.1,45]]]})",
                 R"({"type":"Polygon","coordinates":[[[9,45],[9.1,45],[9,45]]]})",
                 R"({"type":7,"coordinates":[9,45]})",
             })
            EXPECT_THROW(parseCoverageGeometry(json::parse(text)), InvalidArgsException) << text;
    }

    TEST(Coverage, PolygonThatIsNotValidIsRefused)
    {
        IndexFixture index(TEST_NAME);
        insertImage(index.db.get(), "a.jpg", {});
        // A bow tie crosses itself
        CoverageGeometry bowTie;
        bowTie.kind = CoverageGeometry::Kind::Polygon;
        bowTie.parts.push_back({place(-10, -10), place(10, 10), place(10, -10), place(-10, 10), place(-10, -10)});
        EXPECT_THROW(findCoverage(index.db.get(), bowTie, {}), InvalidArgsException);
    }

    // The distances are measured in the UTM zone of the middle of the box of the geometry: the box of a geometry
    // that crosses the antimeridian goes around the world, and its middle is on the other side of it. The
    // geometries are valid: they are refused for their width
    TEST(Coverage, GeometryTooWideIsRefused)
    {
        IndexFixture index(TEST_NAME);
        insertImage(index.db.get(), "a.jpg", {});

        for (const char *text : {
                 R"({"type":"Polygon","coordinates":[[[179.9,44],[-179.9,44],[-179.9,46],[179.9,46],[179.9,44]]]})",
                 R"({"type":"LineString","coordinates":[[179.9,45],[-179.9,45]]})",
                 R"({"type":"Polygon","coordinates":[[[-90.5,44],[90,44],[90,46],[-90.5,46],[-90.5,44]]]})",
                 // 180 degrees on the equator: its ends are 90 degrees from the meridian of the zone of its middle,
                 // where the transverse Mercator projection has no value
                 R"({"type":"Polygon","coordinates":[[[-81,-1],[99,-1],[99,1],[-81,1],[-81,-1]]]})",
             })
            EXPECT_THROW(findCoverage(index.db.get(), parseCoverageGeometry(json::parse(text)), {}), InvalidArgsException) << text;

        // 180 degrees, from 81 W to 99 E, is the widest geometry: its middle is 9 E, the image
        const auto widest = findCoverage(
            index.db.get(),
            parseCoverageGeometry(json::parse(R"({"type":"Polygon","coordinates":[[[-81,44],[99,44],[99,46],[-81,46],[-81,44]]]})")),
            {});
        ASSERT_EQ(widest.size(), 1u);
        EXPECT_EQ(widest[0].path, "a.jpg");
    }

    TEST(Coverage, Options)
    {
        EXPECT_EQ(parseCoverageOptions(json::object()).limit, 20);
        EXPECT_EQ(parseCoverageOptions(json{{"limit", 1}}).limit, 1);
        EXPECT_EQ(parseCoverageOptions(json{{"limit", 100}}).limit, 100);

        for (const char *text : {R"({"limit":0})", R"({"limit":101})", R"({"limit":-3})", R"({"limit":2.5})",
                                 R"({"limit":"3"})", R"({"limit":true})", R"({"limit":null})", R"({"limit":18446744073709551615})",
                                 R"({"lmit":3})", R"({"limit":3,"offset":1})", R"([3])", R"(3)"})
            EXPECT_THROW(parseCoverageOptions(json::parse(text)), InvalidArgsException) << text;
    }

    // ------------------------------------------------------------------ scale

    // 5000 images in a grid of overlapping footprints, and a polygon over a few hundred of them
    TEST(Coverage, FiveThousandImages)
    {
        IndexFixture index(TEST_NAME);
        Transaction tx(index.db.get());
        for (int i = 0; i < 5000; i++)
        {
            Camera camera;
            camera.east = (i % 100) * 100.0;
            camera.north = (i / 100) * 70.0;
            insertImage(index.db.get(), "images/image" + std::to_string(10000 + i) + ".jpg", camera);
        }
        tx.commit();

        CoverageOptions options;
        options.limit = 20;

        // The median time of a search, in milliseconds, and its result
        const auto timed = [&](const CoverageGeometry &geometry, std::vector<CoverageMatch> &matches)
        {
            std::vector<double> milliseconds;
            for (int run = 0; run < 7; run++)
            {
                const auto begin = std::chrono::steady_clock::now();
                matches = findCoverage(index.db.get(), geometry, options);
                milliseconds.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
            }
            std::sort(milliseconds.begin(), milliseconds.end());
            return milliseconds[milliseconds.size() / 2];
        };

        // A 600 x 400 m polygon over the middle of the grid (some tens of footprints), then one over all of it
        std::vector<CoverageMatch> matches;
        const double median = timed(rectangle(4000, 1500, 4600, 1900), matches);
        std::vector<CoverageMatch> overall;
        const double medianOfAll = timed(rectangle(-100, -100, 10000, 3600), overall);
        std::cout << "coverage on 5000 images, median of 7: " << median << " ms for a polygon over some of them, "
                  << medianOfAll << " ms over all of them" << std::endl;

        ASSERT_EQ(matches.size(), 20u);
        EXPECT_TRUE(std::is_sorted(matches.begin(), matches.end(), [](const CoverageMatch &a, const CoverageMatch &b)
                                   { return a.score != b.score ? a.score < b.score : a.path < b.path; }));
        // The best of them is the one whose middle is nearest the middle of the polygon (4300, 1700)
        EXPECT_LT(matches.front().score, 0.3);
        ASSERT_EQ(overall.size(), 20u);
        // The target is 300 ms; a loaded CI machine gets more room
        EXPECT_LT(median, 1000.0);
        EXPECT_LT(medianOfAll, 1000.0);
    }
}
