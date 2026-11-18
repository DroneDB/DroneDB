/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The footprint of a raster, a vector layer and a point cloud that hold a geographic pole. The ring
// of the corners of a polar stereographic raster centred on a pole is a band at the latitude of its
// corners that leaves the pole out (an Antarctic raster was placed between 31.7 S and the equator's
// side of it); such a footprint is every longitude from the pole to the most equatorward latitude of
// its edges. A raster beside the pole, or with a corner at it, keeps the ring of its corners.

#include <memory>
#include <string>
#include <vector>

#include "coordstransformer.h"
#include "entry.h"
#include "gdal_inc.h"
#include "gdal_raii.h"
#include "gtest/gtest.h"
#include "las_test_writer.h"
#include "mio.h"
#include "pointcloud.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"

namespace
{
    using namespace ddb;

    constexpr int SouthPolarStereographic = 3031; // WGS 84 / Antarctic Polar Stereographic
    constexpr int NorthPolarStereographic = 3413; // WGS 84 / NSIDC Sea Ice Polar Stereographic North

    // Longitude and latitude of a point of the CRS epsg
    Point lonLatOf(int epsg, double x, double y)
    {
        const SrsPtr from = srsFromEpsg(epsg);
        const SrsPtr to = srsFromEpsg(4326);
        CoordsTransformer transform(srsOf(from), srsOf(to));
        transform.transform(&x, &y);
        return Point(x, y, 0.0);
    }

    // A one-band raster of size x size pixels of `pixel` metres in the CRS epsg, its upper-left
    // corner at (minX, maxY)
    fs::path writeRaster(const fs::path &path, int epsg, double minX, double maxY, int size, double pixel)
    {
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = size;
        spec.bands = 1;
        spec.epsg = epsg;
        spec.originX = minX;
        spec.originY = maxY;
        spec.pixelSize = pixel;
        return testutils::createGeoRaster(path, spec);
    }

    Entry entryOf(const fs::path &file, const fs::path &folder)
    {
        Entry e;
        parseEntry(file, folder, e, false);
        return e;
    }

    // The footprint is the ring of the cap: north-west, north-east, south-east, south-west corners
    // of the envelope [-180, minLat] x [180, maxLat], closed
    void expectCap(const BasicPolygonGeometry &ring, double minLat, double maxLat)
    {
        ASSERT_EQ(ring.size(), 5);
        const double expected[5][2] = {{-180, maxLat}, {180, maxLat}, {180, minLat}, {-180, minLat}, {-180, maxLat}};
        for (int i = 0; i < 5; i++)
        {
            EXPECT_NEAR(ring.points[i].x, expected[i][0], 1e-9) << "point " << i;
            EXPECT_NEAR(ring.points[i].y, expected[i][1], 1e-9) << "point " << i;
        }
    }

    // The footprint is the ring of the corners of the raster (upper-left, upper-right, lower-right,
    // lower-left, closed), each where the CRS places it
    void expectCornerRing(const BasicPolygonGeometry &ring, int epsg, double minX, double maxY, double extent)
    {
        ASSERT_EQ(ring.size(), 5);
        const double corners[5][2] = {{minX, maxY},
                                      {minX + extent, maxY},
                                      {minX + extent, maxY - extent},
                                      {minX, maxY - extent},
                                      {minX, maxY}};
        for (int i = 0; i < 5; i++)
        {
            const Point expected = lonLatOf(epsg, corners[i][0], corners[i][1]);
            EXPECT_NEAR(ring.points[i].x, expected.x, 1e-9) << "point " << i;
            EXPECT_NEAR(ring.points[i].y, expected.y, 1e-9) << "point " << i;
        }
    }

    TEST(PolarFootprint, aRasterAroundTheSouthPoleCoversTheCapFromThePoleToItsCorners)
    {
        TestArea ta(TEST_NAME, true);
        // 5000 km square centred on the pole: its corners, the farthest points of its edges from the
        // pole, are all at one latitude
        const fs::path file = writeRaster(ta.getPath("south.tif"), SouthPolarStereographic, -2500000, 2500000, 100,
                                          50000);
        const Entry e = entryOf(file, ta.getFolder());
        ASSERT_EQ(e.type, EntryType::GeoRaster);
        const double edge = lonLatOf(SouthPolarStereographic, 2500000, 2500000).y;
        ASSERT_LT(edge, -50);
        expectCap(e.polygon_geom, -90, edge);
    }

    TEST(PolarFootprint, aRasterAroundTheNorthPoleCoversTheCapFromThePoleToItsCorners)
    {
        TestArea ta(TEST_NAME, true);
        // Off-centre: the pole is in the raster, nearer its north-west corner
        const fs::path file = writeRaster(ta.getPath("north.tif"), NorthPolarStereographic, -1000000, 1000000, 100,
                                          30000);
        const Entry e = entryOf(file, ta.getFolder());
        ASSERT_EQ(e.type, EntryType::GeoRaster);
        // The most equatorward point of the edges is the corner farthest from the pole
        const double edge = lonLatOf(NorthPolarStereographic, 2000000, -2000000).y;
        ASSERT_GT(edge, 50);
        expectCap(e.polygon_geom, edge, 90);
    }

    TEST(PolarFootprint, aRasterBesideThePoleKeepsTheRingOfItsCorners)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path file = writeRaster(ta.getPath("beside.tif"), SouthPolarStereographic, 1000000, 2000000, 100,
                                          10000);
        const Entry e = entryOf(file, ta.getFolder());
        ASSERT_EQ(e.type, EntryType::GeoRaster);
        expectCornerRing(e.polygon_geom, SouthPolarStereographic, 1000000, 2000000, 1000000);
    }

    TEST(PolarFootprint, aRasterWithACornerAtThePoleKeepsTheRingOfItsCorners)
    {
        TestArea ta(TEST_NAME, true);
        // A quadrant of the polar grid: its lower-left corner is the pole, which the ring of its
        // corners passes through
        const fs::path file = writeRaster(ta.getPath("quadrant.tif"), SouthPolarStereographic, 0, 2000000, 100,
                                          20000);
        const Entry e = entryOf(file, ta.getFolder());
        ASSERT_EQ(e.type, EntryType::GeoRaster);
        expectCornerRing(e.polygon_geom, SouthPolarStereographic, 0, 2000000, 2000000);
    }

    TEST(PolarFootprint, aVectorLayerAroundTheSouthPoleCoversTheCap)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path file = ta.getPath("antarctica.gpkg");
        {
            GDALDriver *driver = GetGDALDriverManager()->GetDriverByName("GPKG");
            ASSERT_NE(driver, nullptr);
            std::unique_ptr<GDALDataset> ds(driver->Create(file.string().c_str(), 0, 0, 0, GDT_Unknown, nullptr));
            ASSERT_NE(ds, nullptr);
            OGRSpatialReference srs;
            ASSERT_EQ(srs.importFromEPSG(SouthPolarStereographic), OGRERR_NONE);
            OGRLayer *layer = ds->CreateLayer("coast", &srs, wkbPolygon, nullptr);
            ASSERT_NE(layer, nullptr);
            OGRLinearRing ring;
            for (const auto &c : {std::pair{-2000000.0, -2000000.0}, std::pair{2000000.0, -2000000.0},
                                  std::pair{2000000.0, 2000000.0}, std::pair{-2000000.0, 2000000.0},
                                  std::pair{-2000000.0, -2000000.0}})
                ring.addPoint(c.first, c.second);
            OGRPolygon polygon;
            polygon.addRing(&ring);
            OGRFeature feature(layer->GetLayerDefn());
            feature.SetGeometry(&polygon);
            ASSERT_EQ(layer->CreateFeature(&feature), OGRERR_NONE);
        }
        const Entry e = entryOf(file, ta.getFolder());
        ASSERT_EQ(e.type, EntryType::Vector);
        const double edge = lonLatOf(SouthPolarStereographic, 2000000, 2000000).y;
        const auto &layers = e.properties.at("vector").at("layers");
        ASSERT_EQ(layers.size(), 1u);
        const std::vector<double> extent = layers[0].at("extent").get<std::vector<double>>();
        ASSERT_EQ(extent.size(), 4u);
        EXPECT_NEAR(extent[0], -180, 1e-9);
        EXPECT_NEAR(extent[1], -90, 1e-9);
        EXPECT_NEAR(extent[2], 180, 1e-9);
        EXPECT_NEAR(extent[3], edge, 1e-9);
        ASSERT_EQ(e.polygon_geom.size(), 5);
        EXPECT_NEAR(e.polygon_geom.points[0].y, -90, 1e-9);
        EXPECT_NEAR(e.polygon_geom.points[2].y, edge, 1e-9);
    }

    TEST(PolarFootprint, aPointCloudAroundTheSouthPoleCoversTheCap)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path las = ta.getPath("pole.las");
        std::vector<testutils::LasPoint> points;
        for (int i = 0; i <= 10; i++)
            for (int j = 0; j <= 10; j++)
                points.push_back({-100000.0 + 20000.0 * i, -100000.0 + 20000.0 * j, 2800.0});
        const SrsPtr srs = srsFromEpsg(SouthPolarStereographic);
        char *wkt = nullptr;
        ASSERT_EQ(srsOf(srs).exportToWkt(&wkt), OGRERR_NONE);
        testutils::writeLas14(las, takeGdalString(wkt), points);

        PointCloudInfo info;
        ASSERT_TRUE(getPointCloudInfo(io::toUtf8(las), info));
        EXPECT_TRUE(info.footprintWarning.empty()) << info.footprintWarning;
        const double edge = lonLatOf(SouthPolarStereographic, 100000, 100000).y;
        ASSERT_EQ(info.polyBounds.size(), 5);
        // The ring of a point cloud starts at its south-west corner and goes east
        const double expected[5][2] = {{-180, -90}, {180, -90}, {180, edge}, {-180, edge}, {-180, -90}};
        for (int i = 0; i < 5; i++)
        {
            EXPECT_NEAR(info.polyBounds.points[i].x, expected[i][0], 1e-9) << "point " << i;
            EXPECT_NEAR(info.polyBounds.points[i].y, expected[i][1], 1e-6) << "point " << i;
        }
    }
}
