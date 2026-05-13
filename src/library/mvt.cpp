/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "mvt.h"

namespace ddb
{

    int computeMvtMaxZoom(long long featureCount, double extentAreaDeg2)
    {
        if (featureCount <= 0) return 10;
        if (extentAreaDeg2 <= 0.0)
        {
            // Single point / degenerate envelope: use max sensible zoom.
            return 18;
        }

        const double density = static_cast<double>(featureCount) / extentAreaDeg2;

        int maxZoom;
        if (density > 10000.0)      maxZoom = 18;
        else if (density > 1000.0)  maxZoom = 16;
        else if (density > 100.0)   maxZoom = 14;
        else if (density > 10.0)    maxZoom = 12;
        else                        maxZoom = 10;

        if (maxZoom > 18) maxZoom = 18;
        if (maxZoom < 10) maxZoom = 10;
        return maxZoom;
    }

} // namespace ddb
