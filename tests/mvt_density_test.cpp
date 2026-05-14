/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "test.h"
#include "mvt.h"

#include <algorithm>
#include <cmath>

namespace
{

    using namespace ddb;
    // The heuristic is budget-based: targeting kMvtTileBudget MVT tiles for a
    // WGS84 envelope of `areaDeg2` square degrees, with the result clamped
    // to [kMvtMinZoomCap, kMvtMaxZoomCap]. featureCount is unused; it is
    // retained in the signature for ABI stability.

    TEST(testMvtDensity, EmptyOrDegenerateReturnsMax)
    {
        // featureCount <= 0 → max zoom cap (no work to do, no risk).
        EXPECT_EQ(computeMvtMaxZoom(0, 1.0), kMvtMaxZoomCap);
        EXPECT_EQ(computeMvtMaxZoom(-1, 1.0), kMvtMaxZoomCap);
        // Degenerate (zero-area) bbox → max zoom cap.
        EXPECT_EQ(computeMvtMaxZoom(1, 0.0), kMvtMaxZoomCap);
        EXPECT_EQ(computeMvtMaxZoom(1000, 0.0), kMvtMaxZoomCap);
    }

    TEST(testMvtDensity, SmallExtentSaturatesToMax)
    {
        // A tiny envelope produces few tiles even at the max cap, so the
        // formula saturates upward and we get kMvtMaxZoomCap.
        EXPECT_EQ(computeMvtMaxZoom(1,    1.0), kMvtMaxZoomCap);
        EXPECT_EQ(computeMvtMaxZoom(1000, 1.0), kMvtMaxZoomCap);
        EXPECT_EQ(computeMvtMaxZoom(1,    1e-4), kMvtMaxZoomCap);
    }

    TEST(testMvtDensity, FeatureCountIrrelevant)
    {
        // Two very different feature counts on the same envelope must
        // produce the same zoom: cost is bounded by tile count, not features.
        const double area = 360.0 * 180.0; // global
        EXPECT_EQ(computeMvtMaxZoom(1,        area),
                  computeMvtMaxZoom(10000000, area));
    }

    TEST(testMvtDensity, GlobalSparseDatasetIsModerate)
    {
        // World admin boundaries: ~250 features on the global envelope.
        // The old density heuristic produced kMvtMaxZoomCap-ish levels and
        // pathological tile counts (~400k). The budget formula must keep z
        // well below the cap.
        const int z = computeMvtMaxZoom(250, 360.0 * 180.0);
        EXPECT_LT(z, kMvtMaxZoomCap);
        EXPECT_GE(z, kMvtMinZoomCap);
        // Sanity: ratio = budget*64800/64800 = budget → z = floor(0.5*log2(budget)).
        // For budget=50000 this is 7; verify symbolically against the constant.
        const int expected = static_cast<int>(std::floor(
            0.5 * std::log2(static_cast<double>(kMvtTileBudget))));
        EXPECT_EQ(z, std::clamp(expected, kMvtMinZoomCap, kMvtMaxZoomCap));
    }

    TEST(testMvtDensity, BoundsAlwaysClamped)
    {
        // Sweep a wide range to verify return value ∈ [min, max]
        const long long counts[] = {0, 1, 100, 10000, 1000000, 1000000000LL};
        const double areas[] = {0.0, 1e-6, 0.1, 1.0, 100.0, 64800.0, 1e9};
        for (long long c : counts)
        {
            for (double a : areas)
            {
                const int z = computeMvtMaxZoom(c, a);
                EXPECT_GE(z, kMvtMinZoomCap);
                EXPECT_LE(z, kMvtMaxZoomCap);
            }
        }
    }

    TEST(testMvtDensity, MonotonicInExtent)
    {
        // For positive featureCount, larger extent => same-or-lower zoom.
        int prev = kMvtMaxZoomCap;
        for (double a : {1e-3, 1.0, 100.0, 1000.0, 10000.0, 64800.0, 1e6})
        {
            const int z = computeMvtMaxZoom(1000, a);
            EXPECT_LE(z, prev);
            prev = z;
        }
    }

} // namespace
