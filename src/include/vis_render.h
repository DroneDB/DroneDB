/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef VIS_RENDER_H
#define VIS_RENDER_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ddb_export.h"
#include "gdal_inc.h"
#include "raster_stats.h"
#include "thumbs.h"

namespace ddb
{
    /// Pixel window of a raster read into a buffer of outWidth x outHeight
    struct RasterWindow
    {
        int x = 0;
        int y = 0;
        int xsize = 0;
        int ysize = 0;
        int outWidth = 0;
        int outHeight = 0;

        size_t outPixels() const { return static_cast<size_t>(outWidth) * outHeight; }
    };

    /// Parses a "min,max" rescale range. Throws InvalidArgsException when malformed.
    DDB_DLL std::pair<float, float> parseRescaleRange(const std::string &rescale);

    /// Bands to show as R, G, B for visParams: the explicit "bands" list (each checked
    /// against 1..maxBand), the preset's mapping, or the default mapping of the detected
    /// sensor. Empty when nothing selects bands (show the first bands).
    DDB_DLL std::vector<int> resolveVisBands(const std::string &rasterPath, const ThumbVisParams &visParams,
                                             int maxBand);

    /// Display range of a band: mean ± 2.33 σ (≈ p2-p98) clipped to [min, max]
    DDB_DLL std::pair<double, double> stretchRange(const BandStatistics &s);

    /// Renders visParams.formula (a vegetation index) over window of ds with its
    /// colormap: RGBA interleaved, outPixels() * 4 bytes, nodata transparent.
    DDB_DLL std::vector<uint8_t> renderFormulaRgba(GDALDatasetH ds, const std::string &rasterPath,
                                                   const RasterWindow &window, const ThumbVisParams &visParams);
}

#endif // VIS_RENDER_H
