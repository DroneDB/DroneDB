/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "entry.h"

namespace{

using namespace ddb;

TEST(calculateFootprint, Normal) {
    SensorSize sensorSize(36, 24);
    Focal focal(50, 0);
    double relAltitude = 100.0;
    CameraOrientation cameraOri(-60, 0, 30);
    GeoLocation geo(46.842607,-91.99456,198.31);
    BasicPolygonGeometry geom;
    calculateFootprint(sensorSize, geo, focal, cameraOri, relAltitude, geom);
	
	EXPECT_STREQ(geom.toWkt().c_str(), "POLYGONZ ((-91.994308101 46.84345864217 98.31, -91.99431905836 46.84287152156 98.31, -91.99300336858 46.84285995357 98.31, -91.99299239689 46.84344707395 98.31, -91.994308101 46.84345864217 98.31))");
}

}
