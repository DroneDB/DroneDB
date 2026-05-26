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

    TEST(testMvtDensity, GlobalDatasetForcedToOverviewOnly)
    {
        // Full-globe extent: world-scale guard must clamp to kMvtMinZoomCap
        // regardless of feature count or budget. Building MVT tilesets for
        // the entire planet is not a supported use case (each tile clips
        // against every overlapping feature and runtimes explode).
        EXPECT_EQ(computeMvtMaxZoom(250, 360.0 * 180.0), kMvtMinZoomCap);
        EXPECT_EQ(computeMvtMaxZoom(1, 360.0 * 180.0), kMvtMinZoomCap);
        EXPECT_EQ(computeMvtMaxZoom(1000000, 360.0 * 180.0), kMvtMinZoomCap);
        // Just above the threshold is still global-scale.
        const double globalish = kMvtGlobalCoverageThreshold * 360.0 * 180.0;
        EXPECT_EQ(computeMvtMaxZoom(100, globalish), kMvtMinZoomCap);
    }

    TEST(testMvtDensity, LargeButNonGlobalUsesBudgetFormula)
    {
        // Just below the world-scale threshold the budget formula applies.
        // For an extent of (threshold - epsilon) * earthAreaDeg2, the ratio
        // is approximately budget / (threshold - epsilon), so z is
        // floor(0.5 * log2(budget / threshold)) clamped to the bounds.
        const double earthAreaDeg2 = 360.0 * 180.0;
        const double area = (kMvtGlobalCoverageThreshold - 0.01) * earthAreaDeg2;
        const int z = computeMvtMaxZoom(250, area);
        EXPECT_GE(z, kMvtMinZoomCap);
        EXPECT_LE(z, kMvtMaxZoomCap);
        const double ratio = (static_cast<double>(kMvtTileBudget) * earthAreaDeg2) / area;
        const int expected = static_cast<int>(std::floor(0.5 * std::log2(ratio)));
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
