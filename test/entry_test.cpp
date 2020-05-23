/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "../libs/entry.h"

namespace {

TEST(calculateFootprint, Normal) {
    exif::SensorSize sensorSize(36, 24);
    exif::Focal focal(50, 0);
    double relAltitude = 100.0;
    exif::CameraOrientation cameraOri(-60, 0, 30);
    exif::GeoLocation geo(46.842607,-91.99456,198.31);
    entry::BasicPolygonGeometry geom;
    entry::calculateFootprint(sensorSize, geo, focal, cameraOri, relAltitude, geom);

    EXPECT_STREQ(geom.toWkt().c_str(), "");
}

}
