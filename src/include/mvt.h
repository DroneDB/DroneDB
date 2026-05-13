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
     * Maximum number of MVT tiles (cumulative across zoom levels) that the
     * writer is allowed to produce for a single dataset. The MAXZOOM passed
     * to GDAL's MVT writer is derived from this budget and the WGS84 bbox
     * area of the source.
     *
     * Rationale: with the previous density-only heuristic, sparse global
     * datasets (e.g. world admin boundaries with 250 features over the
     * full globe) ended up at MAXZOOM=10, producing ~400k tiles and
     * runtimes in the 10+ minute range. A budget bounds the worst case
     * regardless of how features are distributed.
     */
    constexpr long long kMvtTileBudget = 100000;

    /// Hard lower bound for MVT MAXZOOM (overview-only datasets).
    constexpr int kMvtMinZoomCap = 5;

    /// Hard upper bound for MVT MAXZOOM (avoid pointless detail / huge
    /// per-tile feature counts on densely packed regions).
    constexpr int kMvtMaxZoomCap = 14;

    /**
     * Compute a dynamic MAXZOOM for MVT tiling based on a tile-count budget.
     *
     * The MVT writer materializes tiles for every zoom in [MINZOOM, MAXZOOM]
     * that intersects features. For a bbox of @p extentAreaDeg2 the number
     * of tiles at zoom z is approximately areaDeg2 * 4^z / 64800. We pick
     * the largest z such that this stays within ::kMvtTileBudget, then
     * clamp to [::kMvtMinZoomCap, ::kMvtMaxZoomCap].
     *
     * The @p featureCount parameter is currently unused but kept in the
     * signature for ABI/API stability and to leave room for a future
     * combined heuristic.
     *
     * @param featureCount   Total feature count across all layers (>= 0).
     *                       Currently unused (see rationale above).
     * @param extentAreaDeg2 Area in deg² of the union bounding box (>= 0).
     *                       If 0 (single point / empty), returns ::kMvtMaxZoomCap.
     * @return MAXZOOM in [::kMvtMinZoomCap, ::kMvtMaxZoomCap].
     */
    DDB_DLL int computeMvtMaxZoom(long long featureCount, double extentAreaDeg2);

} // namespace ddb

#endif // MVT_H
