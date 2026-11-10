/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include <limits>
#include "geo.h"

namespace
{

    using namespace ddb;

    TEST(testUTM, Normal)
    {
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

    // The envelope a WGS 84 footprint may have (FR-SHR-03): finite, inside [-180, 180] x [-90, 90],
    // min <= max. Literal values: UTM metres, NaN, reversed and out-of-range ones all fail.
    TEST(isWgs84Envelope, table)
    {
        const double inf = std::numeric_limits<double>::infinity();
        const double nan = std::numeric_limits<double>::quiet_NaN();
        struct Row
        {
            const char *name;
            double minX, minY, maxX, maxY;
            bool expected;
        };
        const Row rows[] = {
            {"a footprint in degrees", -91.995, 46.842, -91.993, 46.843, true},
            {"the whole world", -180, -90, 180, 90, true},
            {"a point", 10, 20, 10, 20, true},
            {"a line along the equator", -5, 0, 5, 0, true},
            {"UTM metres", 576764.77, 5188207.22, 576864.77, 5188307.22, false},
            {"Web Mercator metres", -10018754.17, 0, 10018754.17, 5621521.49, false},
            {"longitude below -180", -180.0001, 0, 0, 1, false},
            {"longitude above 180", 0, 0, 180.0001, 1, false},
            {"latitude below -90", 0, -90.0001, 1, 0, false},
            {"latitude above 90", 0, 0, 1, 90.0001, false},
            {"min x above max x", 10, 0, 9, 1, false},
            {"min y above max y", 0, 10, 1, 9, false},
            {"NaN minX", nan, 0, 1, 1, false},
            {"NaN minY", 0, nan, 1, 1, false},
            {"NaN maxX", 0, 0, nan, 1, false},
            {"NaN maxY", 0, 0, 1, nan, false},
            {"infinite maxX", 0, 0, inf, 1, false},
            {"negative infinite minY", 0, -inf, 1, 1, false},
        };
        for (const auto &r : rows)
            EXPECT_EQ(isWgs84Envelope(r.minX, r.minY, r.maxX, r.maxY), r.expected) << r.name;
    }

}
