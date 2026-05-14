/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "mvt.h"

#include <algorithm>
#include <cmath>

namespace ddb
{

    int computeMvtMaxZoom(long long featureCount, double extentAreaDeg2)
    {
        // Degenerate envelope (single point / empty bbox) or an explicitly
        // empty layer: use the cap zoom — no real work to bound.
        //
        // NOTE: featureCount may legitimately be negative when the source
        // driver could not provide a cheap count (OGR convention: -1 means
        // "unknown"). In that case we must NOT short-circuit; the area-based
        // heuristic is still meaningful and prevents pathological tile
        // counts on large extents with unknown feature counts.
        if (extentAreaDeg2 <= 0.0 || featureCount == 0)
            return kMvtMaxZoomCap;

        // Approximate number of MVT tiles intersecting the WGS84 bbox at zoom z:
        //
        //     tiles(z) ≈ areaDeg2 * 4^z / earthAreaDeg2
        //
        // where earthAreaDeg2 = 360 * 180 = 64800. Solving tiles(z) == budget
        // for z gives the largest zoom that fits the budget:
        //
        //     z = floor( 0.5 * log2(budget * earthAreaDeg2 / areaDeg2) )
        //
        // Rationale: featureCount is intentionally unused. The dominant cost
        // is the number of tiles produced by GDAL's MVT writer, which is
        // bounded by bbox coverage, not feature count. Density-based
        // heuristics underrate sparse global datasets (e.g. world admin
        // boundaries) and produce pathological tile counts.
        constexpr double earthAreaDeg2 = 360.0 * 180.0;
        const double ratio = (static_cast<double>(kMvtTileBudget) * earthAreaDeg2)
                             / extentAreaDeg2;
        // ratio > 0 here (budget>0, earthAreaDeg2>0, extentAreaDeg2>0).
        const int z = static_cast<int>(std::floor(0.5 * std::log2(ratio)));

        return std::clamp(z, kMvtMinZoomCap, kMvtMaxZoomCap);
    }

} // namespace ddb
