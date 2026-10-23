/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-06: the small geo and raster primitives (angle factors, nodata rule, GeoTIFF options, DEM
// opening, coordinate transformer axis order) on their own.

#include <cmath>

#include "gtest/gtest.h"

#include "mathutil.h"

namespace
{
    using namespace ddb;

    // DQ-05 left the private `kPi / 180.0` factors of stockpile, volume and raster_profile alone
    // because deg2rad(x) = (x * kPi) / 180.0 rounds differently. DQ-06 replaces them with
    // kDegToRad, which is that same factor, so no output changes; and the expressions that were
    // written as (x * kPi) / 180.0 or (x * 180.0) / kPi became deg2rad / rad2deg.
    TEST(Dq06Math, theTwoWaysToConvertDegreesDifferInTheLastBit)
    {
        int differing = 0;
        for (int i = 0; i <= 3600; i++)
        {
            const double degrees = i * 0.1;
            if (degrees * kDegToRad != deg2rad(degrees))
                differing++;
        }
        EXPECT_GT(differing, 0) << "if this ever fails the two forms agree and kDegToRad is redundant";
        EXPECT_EQ(kDegToRad, kPi / 180.0);
        EXPECT_EQ(kRadToDeg, 180.0 / kPi);
    }

    TEST(Dq06Math, deg2radAndRad2degAreTheExpressionsTheyReplaced)
    {
        for (int i = -3600; i <= 3600; i++)
        {
            const double x = i * 0.1234567;
            EXPECT_EQ(deg2rad(x), x * kPi / 180.0);  // merge_multispectral, tiler
            EXPECT_EQ(rad2deg(x), x * 180.0 / kPi);  // align
            EXPECT_EQ(kRadToDeg * x, 180.0 / kPi * x); // tiler
        }
    }
}
