/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef VIS_RENDER_H
#define VIS_RENDER_H

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ddb_export.h"
#include "gdal_inc.h"
#include "raster_stats.h"
#include "raster_utils.h"
#include "thumbs.h"
#include "vegetation.h"

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

    /// What the render of a formula over one raster decides once: the formula, the band filter
    /// that maps R, G, B, N, Re and T to bands, the colormap and the nodata of every band. The
    /// thumbnails, tiles, statistics and the export of a formula all start from one.
    struct FormulaPlan
    {
        const VegetationFormula *formula = nullptr;
        BandFilter filter;
        const Colormap *colormap = nullptr; // null when the plan was made without one
        RasterBandNodata nodata;            // per band, and the alpha band
        int bandCount = 0;
    };

    /// Resolves formulaId, the band filter (bandFilter, "" = detected from the sensor profile of
    /// rasterPath) and, with withColormap, the colormap (colormapId, "" = "rdylgn"). Throws
    /// InvalidArgsException for an unknown formula or colormap.
    DDB_DLL FormulaPlan planFormula(GDALDatasetH ds, const std::string &rasterPath, const std::string &formulaId,
                                    const std::string &bandFilter, const std::string &colormapId, bool withColormap);

    /// The same with the band filter already resolved
    DDB_DLL FormulaPlan planFormula(GDALDatasetH ds, const std::string &formulaId, const BandFilter &filter,
                                    const std::string &colormapId, bool withColormap);

    /// The buffers a formula computation reuses from window to window
    struct FormulaBuffers
    {
        std::vector<std::vector<float>> bands; // one per band
        std::vector<float *> ptrs;
        std::vector<float> result;
    };

    /// Reads window of every band as float, masks the transparent and nodata pixels and applies
    /// the formula: buffers.result holds window.outPixels() values, NODATA_SENTINEL where a pixel
    /// has none. The read stops with the call that is canceled (DQ-15).
    DDB_DLL void computeFormulaValues(GDALDatasetH ds, const FormulaPlan &plan, const RasterWindow &window,
                                      FormulaBuffers &buffers);

    /// A computed value that is a number to render: neither the nodata sentinel nor NaN
    inline bool isFormulaValue(float v) { return v != NODATA_SENTINEL && !std::isnan(v); }

    /// The display range that needs no pixels: the explicit "min,max" rescale (InvalidArgsException
    /// when malformed), else the range of the formula itself; none when neither is given
    DDB_DLL std::optional<std::pair<float, float>> fixedDisplayRange(const std::string &rescale,
                                                                     const VegetationFormula &formula);

    /// The display range of values that have no fixed range: their p2 and p98 (values are sorted),
    /// (0, 1) when there are none
    DDB_DLL std::pair<float, float> percentileDisplayRange(std::vector<float> &values);

    /// Renders visParams.formula (a vegetation index) over window of ds with its
    /// colormap: RGBA interleaved, outPixels() * 4 bytes, nodata transparent.
    DDB_DLL std::vector<uint8_t> renderFormulaRgba(GDALDatasetH ds, const std::string &rasterPath,
                                                   const RasterWindow &window, const ThumbVisParams &visParams);
}

#endif // VIS_RENDER_H
