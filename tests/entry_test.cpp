/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "entry.h"
#include "test.h"
#include "testarea.h"

namespace
{

    using namespace ddb;

    TEST(calculateFootprint, Normal)
    {
        SensorSize sensorSize(36, 24);
        Focal focal(50, 0);
        double relAltitude = 100.0;
        CameraOrientation cameraOri(-60, 0, 30);
        GeoLocation geo(46.842607, -91.99456, 198.31);
        BasicPolygonGeometry geom;
        calculateFootprint(sensorSize, geo, focal, cameraOri, relAltitude, geom);

        EXPECT_STREQ(geom.toWkt().c_str(), "POLYGONZ ((-91.994308101 46.84345864217 98.31, -91.99431905836 46.84287152156 98.31, -91.99300336858 46.84285995357 98.31, -91.99299239689 46.84344707395 98.31, -91.994308101 46.84345864217 98.31))");
    }

    TEST(parseJsonGeometries, Normal)
    {
        Entry e;
        e.parsePointGeometry("[1, 2, 3]");
        EXPECT_EQ(e.point_geom.size(), 1);

        e.parsePolygonGeometry("[[[1, 2], [3,4]]]");
        EXPECT_EQ(e.polygon_geom.size(), 2);
    }

    TEST(parseEntry, EPSG2193)
    {
        TestArea ta(TEST_NAME);
        fs::path pc = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/refs/heads/master/ortho/wro.tif",
                                           "wro.tif");

        Entry e;

        parseEntry(pc, ta.getFolder(), e, false);

        /* Expected values

        Point Geometry: [[-41.06625411739, 175.4035257699, 0] ]
Polygon Geometry: [[-41.06584339802, 175.4029416126, 0] [-41.06581965903, 175.4040791346, 0] [-41.06666483358, 175.4041099344, 0] [-41.06668857327, 175.4029723979, 0] [-41.06584339802, 175.4029416126, 0] ]

        */

        EXPECT_EQ(e.point_geom.size(), 1);
        EXPECT_EQ(e.polygon_geom.size(), 5);
        EXPECT_NEAR(e.point_geom.getPoint(0).x, 175.4035257699, 1e-9);
        EXPECT_NEAR(e.point_geom.getPoint(0).y, -41.06625411739, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(0).x, 175.4029416126, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(0).y, -41.06584339802, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(1).x, 175.4040791346, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(1).y, -41.06581965903, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(2).x, 175.4041099344, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(2).y, -41.06666483358, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(3).x, 175.4029723979, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(3).y, -41.06668857327, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(4).x, 175.4029416126, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(4).y, -41.06584339802, 1e-9);

    }

    TEST(parseEntry, EPSG32611)
    {
        TestArea ta(TEST_NAME);
        fs::path pc = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/refs/heads/master/ortho/copr.tif",
                                           "copr.tif");

        Entry e;

        parseEntry(pc, ta.getFolder(), e, false);

        /* Expected values

Point Geometry: [[-119.8801994362, 34.40849815034, 0] ]
Polygon Geometry: [[-119.8804248213, 34.40867109444, 0] [-119.8799862706, 34.40868142837, 0] [-119.8799740521, 34.40832520577, 0] [-119.8804126009, 34.40831487198, 0] [-119.8804248213, 34.40867109444, 0] ]

        */

        EXPECT_EQ(e.point_geom.size(), 1);
        EXPECT_EQ(e.polygon_geom.size(), 5);
        EXPECT_NEAR(e.point_geom.getPoint(0).x, -119.8801994362, 1e-9);
        EXPECT_NEAR(e.point_geom.getPoint(0).y, 34.40849815034, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(0).x, -119.8804248213, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(0).y, 34.40867109444, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(1).x, -119.8799862706, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(1).y, 34.40868142837, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(2).x, -119.8799740521, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(2).y, 34.40832520577, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(3).x, -119.8804126009, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(3).y, 34.40831487198, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(4).x, -119.8804248213, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(4).y, 34.40867109444, 1e-9);

    }


}
