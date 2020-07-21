/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "geo.h"

namespace {

using namespace ddb;

TEST(testUTM, Normal) {
    double latitude = 46.842979268105516;
    double longitude = -91.99321949277439;
    UTMZone zone = getUTMZone(latitude, longitude);
    EXPECT_EQ(zone.zone, 15);
    EXPECT_EQ(zone.north, true);

    Projected2D utm = toUTM(latitude, longitude, zone);
    EXPECT_NEAR(utm.x, 576764.77, 1E-2);
    EXPECT_NEAR(utm.y, 5188207.22, 1E-2);

    Geographic2D coords = fromUTM(utm, zone);
    EXPECT_NEAR(coords.latitude, latitude, 1E-10);
    EXPECT_NEAR(coords.longitude, longitude, 1E-10);
}

}
