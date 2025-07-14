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

    TEST(parseEntry, wro_EPSG2193)
    {
        TestArea ta(TEST_NAME);
        fs::path pc = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/refs/heads/master/ortho/wro.tif",
                                           "wro.tif");

        Entry e;

        parseEntry(pc, ta.getFolder(), e, false);

        /* Expected values

        Point Geometry: [[175.403526, -41.066254, 0] ]
        Polygon Geometry: [[175.4029416126, -41.06584339802, 0] [175.4040791346, -41.06581965903, 0] [175.4041099344, -41.06666483358, 0] [175.4029723979, -41.06668857327, 0] [175.4029416126, -41.06584339802, 0] ]

        */

        EXPECT_EQ(e.point_geom.size(), 1);
        EXPECT_EQ(e.polygon_geom.size(), 5);
        EXPECT_NEAR(e.point_geom.getPoint(0).x, 175.403526, 1e-5);
        EXPECT_NEAR(e.point_geom.getPoint(0).y, -41.066254, 1e-5);
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

    TEST(parseEntry, copr_EPSG32611)
    {
        TestArea ta(TEST_NAME);
        fs::path pc = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/refs/heads/master/ortho/copr.tif",
                                           "copr.tif");

        Entry e;

        parseEntry(pc, ta.getFolder(), e, false);

        /* Expected values

        Point Geometry: [[-119.880199, 34.408498, 0] ]
        Polygon Geometry: [[-119.8804248213, 34.40867109444, 0] [-119.8799862706, 34.40868142837, 0] [-119.8799740521, 34.40832520577, 0] [-119.8804126009, 34.40831487198, 0] [-119.8804248213, 34.40867109444, 0] ]

        */

        EXPECT_EQ(e.point_geom.size(), 1);
        EXPECT_EQ(e.polygon_geom.size(), 5);
        EXPECT_NEAR(e.point_geom.getPoint(0).x, -119.880199, 1e-5);
        EXPECT_NEAR(e.point_geom.getPoint(0).y, 34.408498, 1e-5);
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

    TEST(parseEntry, mygla_EPSG4326)
    {
        TestArea ta(TEST_NAME);
        fs::path pc = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/refs/heads/master/ortho/mygla.tif",
                                           "mygla.tif");

        Entry e;

        parseEntry(pc, ta.getFolder(), e, false);

        /* Expected values

        Point Geometry: [[18.873164, 49.593847, 0] ]
        Polygon Geometry: [[18.87265311725, 49.59426247208, 0] [18.87363777003, 49.59428057246, 0] [18.87367465697, 49.59343247122, 0] [18.87269002125, 49.59341437138, 0] [18.87265311725, 49.59426247208, 0] ]

        */

        EXPECT_EQ(e.point_geom.size(), 1);
        EXPECT_EQ(e.polygon_geom.size(), 5);
        EXPECT_NEAR(e.point_geom.getPoint(0).x, 18.873164, 1e-5);
        EXPECT_NEAR(e.point_geom.getPoint(0).y, 49.593847, 1e-5);
        EXPECT_NEAR(e.polygon_geom.getPoint(0).x, 18.87265311725, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(0).y, 49.59426247208, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(1).x, 18.87363777003, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(1).y, 49.59428057246, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(2).x, 18.87367465697, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(2).y, 49.59343247122, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(3).x, 18.87269002125, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(3).y, 49.59341437138, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(4).x, 18.87265311725, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(4).y, 49.59426247208, 1e-9);

    }

    TEST(parseEntry, aukerman_EPSG32617)
    {
        TestArea ta(TEST_NAME);
        fs::path pc = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/refs/heads/master/ortho/aukerman.tif",
                                           "aukerman.tif");

        Entry e;

        parseEntry(pc, ta.getFolder(), e, false);

        /* Expected values - UTM Zone 17N (EPSG:32617)
        Point Geometry: [[-81.752308, 41.30423, 0] ]
        Polygon Geometry: [[-81.75439362623, 41.30546424327, 0] [-81.75025051315, 41.30549132211, 0] [-81.75022189939, 41.3029949697, 0] [-81.75436485449, 41.30296789322, 0] [-81.75439362623, 41.30546424327, 0] ]
        */

        EXPECT_EQ(e.point_geom.size(), 1);
        EXPECT_EQ(e.polygon_geom.size(), 5);
        EXPECT_NEAR(e.point_geom.getPoint(0).x, -81.752308, 1e-5);
        EXPECT_NEAR(e.point_geom.getPoint(0).y, 41.30423, 1e-5);
        EXPECT_NEAR(e.polygon_geom.getPoint(0).x, -81.75439362623, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(0).y, 41.30546424327, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(1).x, -81.75025051315, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(1).y, 41.30549132211, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(2).x, -81.75022189939, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(2).y, 41.3029949697, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(3).x, -81.75436485449, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(3).y, 41.30296789322, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(4).x, -81.75439362623, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(4).y, 41.30546424327, 1e-9);
    }

    TEST(parseEntry, brighton_beach_EPSG32615)
    {
        TestArea ta(TEST_NAME);
        fs::path pc = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/refs/heads/master/ortho/brighton-beach.tif",
                                           "brighton-beach.tif");

        Entry e;

        parseEntry(pc, ta.getFolder(), e, false);

        /* Expected values - UTM Zone 15N (EPSG:32615)
        Point Geometry: [[-91.99394, 46.842566, 0] ]
        Polygon Geometry: [[-91.99475648454, 46.8430133003, 0] [-91.99310713023, 46.84299880252, 0] [-91.99312356984, 46.84211896052, 0] [-91.99477289722, 46.84213345785, 0] [-91.99475648454, 46.8430133003, 0] ]
        */

        EXPECT_EQ(e.point_geom.size(), 1);
        EXPECT_EQ(e.polygon_geom.size(), 5);
        EXPECT_NEAR(e.point_geom.getPoint(0).x, -91.99394, 1e-5);
        EXPECT_NEAR(e.point_geom.getPoint(0).y, 46.842566, 1e-5);
        EXPECT_NEAR(e.polygon_geom.getPoint(0).x, -91.99475648454, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(0).y, 46.8430133003, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(1).x, -91.99310713023, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(1).y, 46.84299880252, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(2).x, -91.99312356984, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(2).y, 46.84211896052, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(3).x, -91.99477289722, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(3).y, 46.84213345785, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(4).x, -91.99475648454, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(4).y, 46.8430133003, 1e-9);
    }

    TEST(parseEntry, caliterra_EPSG32614)
    {
        TestArea ta(TEST_NAME);
        fs::path pc = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/refs/heads/master/ortho/caliterra.tif",
                                           "caliterra.tif");

        Entry e;

        parseEntry(pc, ta.getFolder(), e, false);

        /* Expected values - UTM Zone 14N (EPSG:32614)
        Point Geometry: [[-98.090126, 30.171261, 0] ]
        Polygon Geometry: [[-98.09102270717, 30.17211432868, 0] [-98.08921414207, 30.17210178537, 0] [-98.08922972589, 30.17040706833, 0] [-98.09103826006, 30.17041961079, 0] [-98.09102270717, 30.17211432868, 0] ]
        */

        EXPECT_EQ(e.point_geom.size(), 1);
        EXPECT_EQ(e.polygon_geom.size(), 5);
        EXPECT_NEAR(e.point_geom.getPoint(0).x, -98.090126, 1e-5);
        EXPECT_NEAR(e.point_geom.getPoint(0).y, 30.171261, 1e-5);
        EXPECT_NEAR(e.polygon_geom.getPoint(0).x, -98.09102270717, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(0).y, 30.17211432868, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(1).x, -98.08921414207, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(1).y, 30.17210178537, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(2).x, -98.08922972589, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(2).y, 30.17040706833, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(3).x, -98.09103826006, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(3).y, 30.17041961079, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(4).x, -98.09102270717, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(4).y, 30.17211432868, 1e-9);
    }

    TEST(parseEntry, sheffield_park_3_EPSG32617)
    {
        TestArea ta(TEST_NAME);
        fs::path pc = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/refs/heads/master/ortho/sheffield-park-3.tif",
                                           "sheffield-park-3.tif");

        Entry e;

        parseEntry(pc, ta.getFolder(), e, false);

        /* Expected values - UTM Zone 17N (EPSG:32617)
        Point Geometry: [[-82.696613, 28.039156, 0] ]
        Polygon Geometry: [[-82.69767134961, 28.03991305122, 0] [-82.69557883662, 28.03993890273, 0] [-82.69555469847, 28.03839980141, 0] [-82.69764718171, 28.03837395156, 0] [-82.69767134961, 28.03991305122, 0] ]
        */

        EXPECT_EQ(e.point_geom.size(), 1);
        EXPECT_EQ(e.polygon_geom.size(), 5);
        EXPECT_NEAR(e.point_geom.getPoint(0).x, -82.696613, 1e-5);
        EXPECT_NEAR(e.point_geom.getPoint(0).y, 28.039156, 1e-5);
        EXPECT_NEAR(e.polygon_geom.getPoint(0).x, -82.69767134961, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(0).y, 28.03991305122, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(1).x, -82.69557883662, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(1).y, 28.03993890273, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(2).x, -82.69555469847, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(2).y, 28.03839980141, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(3).x, -82.69764718171, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(3).y, 28.03837395156, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(4).x, -82.69767134961, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(4).y, 28.03991305122, 1e-9);
    }

    TEST(parseEntry, vo_EPSG31370)
    {
        TestArea ta(TEST_NAME);
        fs::path pc = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/refs/heads/master/ortho/vo.tif",
                                           "vo.tif");

        Entry e;

        parseEntry(pc, ta.getFolder(), e, false);

        // Point Geometry (centro)
        EXPECT_NEAR(e.point_geom.getPoint(0).x, 4.343966, 1e-5);
        EXPECT_NEAR(e.point_geom.getPoint(0).y, 50.691592, 1e-5);

        // Polygon Geometry (angoli - ordine: UL, UR, LR, LL, UL)
        EXPECT_NEAR(e.polygon_geom.getPoint(0).x, 4.342762755994, 1e-9); // Upper Left
        EXPECT_NEAR(e.polygon_geom.getPoint(0).y, 50.69212694232, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(1).x, 4.345168867205, 1e-9); // Upper Right
        EXPECT_NEAR(e.polygon_geom.getPoint(1).y, 50.69212743836, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(2).x, 4.345169386691, 1e-9); // Lower Right
        EXPECT_NEAR(e.polygon_geom.getPoint(2).y, 50.69105730148, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(3).x, 4.342763330072, 1e-9); // Lower Left
        EXPECT_NEAR(e.polygon_geom.getPoint(3).y, 50.69105680545, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(4).x, 4.342762755994, 1e-9); // Chiusura (UL)
        EXPECT_NEAR(e.polygon_geom.getPoint(4).y, 50.69212694232, 1e-9);

    }

    TEST(parseEntry, w5s_EPSG32615)
    {
        TestArea ta(TEST_NAME);
        fs::path pc = ta.downloadTestAsset("https://github.com/DroneDB/test_data/raw/refs/heads/master/ortho/w5s.tif",
                                           "w5s.tif");

        Entry e;

        parseEntry(pc, ta.getFolder(), e, false);

        /* Expected values - UTM Zone 15N (EPSG:32615)
        Point Geometry: [[-95.20157904877, 42.64467943235, 0] ]
        Polygon Geometry: [[-95.20184723851, 42.64493730274, 0] [-95.20132940448, 42.6449472591, 0] [-95.20131086128, 42.64442156127, 0] [-95.20182869095, 42.64441160509, 0] [-95.20184723851, 42.64493730274, 0] ]
        */

        EXPECT_EQ(e.point_geom.size(), 1);
        EXPECT_EQ(e.polygon_geom.size(), 5);
        EXPECT_NEAR(e.point_geom.getPoint(0).x, -95.201579, 1e-5);
        EXPECT_NEAR(e.point_geom.getPoint(0).y, 42.644679, 1e-5);
        EXPECT_NEAR(e.polygon_geom.getPoint(0).x, -95.20184723851, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(0).y, 42.64493730274, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(1).x, -95.20132940448, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(1).y, 42.6449472591, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(2).x, -95.20131086128, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(2).y, 42.64442156127, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(3).x, -95.20182869095, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(3).y, 42.64441160509, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(4).x, -95.20184723851, 1e-9);
        EXPECT_NEAR(e.polygon_geom.getPoint(4).y, 42.64493730274, 1e-9);

    }

}
