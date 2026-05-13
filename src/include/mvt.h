/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef MVT_H
#define MVT_H

#include <string>
#include "ddb_export.h"

namespace ddb
{

    /**
     * Compute a dynamic MAXZOOM for MVT tiling based on feature density.
     *
     * Heuristic (features per square degree in WGS84):
     *   density >10000 → 18
     *   density >1000  → 16
     *   density >100   → 14
     *   density >10    → 12
     *   otherwise      → 10
     *
     * @param featureCount  Total feature count across all layers (>= 0).
     * @param extentAreaDeg2 Area in deg² of the union bounding box (>= 0).
     *                       If 0 (single point or empty), returns 18.
     * @return MAXZOOM in [10, 18].
     */
    DDB_DLL int computeMvtMaxZoom(long long featureCount, double extentAreaDeg2);

} // namespace ddb

#endif // MVT_H
