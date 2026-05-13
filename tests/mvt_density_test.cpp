/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "test.h"
#include "mvt.h"

namespace
{

    using namespace ddb;

    // Boundary conditions
    TEST(testMvtDensity, EmptyDataset)
    {
        // featureCount <= 0 → floor (10)
        EXPECT_EQ(computeMvtMaxZoom(0, 1.0), 10);
        EXPECT_EQ(computeMvtMaxZoom(-1, 1.0), 10);
    }

    TEST(testMvtDensity, DegenerateExtent)
    {
        // Single point (area 0) but with features → maximum sensible zoom (18)
        EXPECT_EQ(computeMvtMaxZoom(1, 0.0), 18);
        EXPECT_EQ(computeMvtMaxZoom(1000, 0.0), 18);
    }

    TEST(testMvtDensity, LowDensity)
    {
        // 1 feature on a degree² → density 1 → floor 10
        EXPECT_EQ(computeMvtMaxZoom(1, 1.0), 10);
        // 10 features on a degree² → density 10 (not > 10) → still 10
        EXPECT_EQ(computeMvtMaxZoom(10, 1.0), 10);
    }

    TEST(testMvtDensity, MediumDensity)
    {
        // density 11 → zoom 12
        EXPECT_EQ(computeMvtMaxZoom(11, 1.0), 12);
        // density 101 → zoom 14
        EXPECT_EQ(computeMvtMaxZoom(101, 1.0), 14);
        // density 1001 → zoom 16
        EXPECT_EQ(computeMvtMaxZoom(1001, 1.0), 16);
    }

    TEST(testMvtDensity, HighDensity)
    {
        // density 100001 → 18 (cap)
        EXPECT_EQ(computeMvtMaxZoom(100001, 1.0), 18);
        // density 10001 → 18
        EXPECT_EQ(computeMvtMaxZoom(10001, 1.0), 18);
    }

    TEST(testMvtDensity, GlobalSparseDataset)
    {
        // 250 features on a global envelope (~64800 deg²) → density ~0.004
        // → floor 10 (this is the "ita" worldwide shapefile case).
        EXPECT_EQ(computeMvtMaxZoom(250, 360.0 * 180.0), 10);
    }

    TEST(testMvtDensity, BoundsAlwaysClamped)
    {
        // Sweep a wide range to verify return value ∈ [10, 18]
        const long long counts[] = {0, 1, 100, 10000, 1000000, 1000000000LL};
        const double areas[] = {0.0, 1e-6, 0.1, 1.0, 100.0, 64800.0};
        for (long long c : counts)
        {
            for (double a : areas)
            {
                const int z = computeMvtMaxZoom(c, a);
                EXPECT_GE(z, 10);
                EXPECT_LE(z, 18);
            }
        }
    }

} // namespace
