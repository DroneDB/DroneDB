/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DDB_TILES3D_H
#define DDB_TILES3D_H

#include <string>
#include "ddb_export.h"

namespace ddb
{

    /**
     * @brief WGS84 footprint and georeferencing information of an OGC 3D Tiles archive.
     *
     * Derived from the root tile boundingVolume of the tileset.json found at the root
     * of a .3tz (ZIP) archive. A tileset is considered georeferenced when its bounding
     * volume is a WGS84 region, or when its (optionally transformed) box/sphere centre
     * lands on the Earth ellipsoid (ECEF), as opposed to a local engineering frame.
     */
    struct Tiles3DInfo
    {
        bool georeferenced = false;                     ///< True when the tileset sits on the globe (ECEF/region).
        bool hasBounds = false;                         ///< True when the WGS84 footprint below is valid.
        double west = 0.0, south = 0.0, east = 0.0, north = 0.0; ///< WGS84 footprint (degrees).
        double centerLon = 0.0, centerLat = 0.0, centerAlt = 0.0; ///< Footprint centre (degrees / meters).
        std::string assetVersion;                       ///< 3D Tiles asset version ("1.0" / "1.1").
        double geometricError = 0.0;                    ///< Root geometric error (meters).
    };

    /**
     * @brief Read tileset.json from inside a .3tz archive and derive its footprint.
     *
     * Opens the .3tz (ZIP) read-only, extracts only the root `tileset.json` in memory
     * and inspects `root.boundingVolume` (region / box / sphere, combined with an
     * optional `root.transform`) to compute a WGS84 footprint and a georeferenced flag.
     *
     * Best-effort: returns false (leaving @p info untouched) when the archive cannot be
     * opened, `tileset.json` is missing/invalid, or the bounding volume is not usable,
     * so a .3tz can still be indexed without a footprint instead of failing the parse.
     *
     * @param ttzPath Path to the .3tz archive.
     * @param info Output footprint/georeferencing info (only written on success).
     * @return true when a usable footprint was computed, false otherwise.
     */
    DDB_DLL bool getTiles3DInfo(const std::string &ttzPath, Tiles3DInfo &info);

}

#endif // DDB_TILES3D_H
