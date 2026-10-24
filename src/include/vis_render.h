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
    std::pair<float, float> parseRescaleRange(const std::string &rescale);

    /// Bands to show as R, G, B for visParams: the explicit "bands" list (each checked
    /// against 1..maxBand), the preset's mapping, or the default mapping of the detected
    /// sensor. Empty when nothing selects bands (show the first bands).
    std::vector<int> resolveVisBands(const std::string &rasterPath, const ThumbVisParams &visParams,
                                     int maxBand);

    /// Display range of a band: mean ± 2.33 σ (≈ p2-p98) clipped to [min, max]
    std::pair<double, double> stretchRange(const BandStatistics &s);

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
    FormulaPlan planFormula(GDALDatasetH ds, const std::string &rasterPath, const std::string &formulaId,
                            const std::string &bandFilter, const std::string &colormapId, bool withColormap);

    /// The same with the band filter already resolved
    FormulaPlan planFormula(GDALDatasetH ds, const std::string &formulaId, const BandFilter &filter,
                            const std::string &colormapId, bool withColormap);

    /// Which bands decide that a pixel has no value, when a formula is computed
    enum class NodataMask
    {
        /// A pixel with no data (nodata, NaN) in ANY band of the raster, or transparent in the alpha
        /// band, is left out. The rule of the thumbnails, the tiles and the formula renders: the
        /// same pixels are blank whichever formula is shown.
        AnyBand,
        /// Only the bands the formula reads (and the alpha band, which says "no data here" for every
        /// band) decide. A pixel whose only gap is in a band the formula does not read is computed,
        /// and the bands the formula does not read are not even read. The rule of the
        /// vegetation-index map layer. Not for the thermal passthroughs (CELSIUS, KELVIN), which read
        /// band 0 whatever the band filter says.
        BandsRead
    };

    /// The buffers a formula computation reuses from window to window
    struct FormulaBuffers
    {
        std::vector<std::vector<float>> bands; // one per band
        std::vector<float *> ptrs;
        std::vector<float> result;
    };

    /// Reads window of every band as float, masks the transparent and nodata pixels and applies
    /// the formula: buffers.result holds window.outPixels() values, NODATA_SENTINEL where a pixel
    /// has none, the pixels that mask being those of `mask`. With BandsRead the bands that are not
    /// read have a null entry in buffers.ptrs. The read stops with the call that is canceled (DQ-15).
    void computeFormulaValues(GDALDatasetH ds, const FormulaPlan &plan, const RasterWindow &window,
                              FormulaBuffers &buffers, NodataMask mask = NodataMask::AnyBand);

    /// A computed value that is a number to render: neither the nodata sentinel nor NaN
    inline bool isFormulaValue(float v) { return v != NODATA_SENTINEL && !std::isnan(v); }

    /// The display range that needs no pixels: the explicit "min,max" rescale (InvalidArgsException
    /// when malformed), else the range of the formula itself; none when neither is given
    std::optional<std::pair<float, float>> fixedDisplayRange(const std::string &rescale,
                                                             const VegetationFormula &formula);

    /// The display range of values that have no fixed range: their p2 and p98 (values are sorted),
    /// (0, 1) when there are none
    std::pair<float, float> percentileDisplayRange(std::vector<float> &values);

    /// Renders the planned formula (the plan has a colormap) over window of ds: RGBA interleaved,
    /// outPixels() * 4 bytes, nodata transparent. The display range is the explicit "min,max"
    /// rescale, else the range of the formula, else the p2-p98 of the computed values. `mask` is
    /// the nodata rule of computeFormulaValues.
    std::vector<uint8_t> renderPlannedFormulaRgba(GDALDatasetH ds, const FormulaPlan &plan,
                                                  const RasterWindow &window, const std::string &rescale,
                                                  NodataMask mask = NodataMask::AnyBand);

    /// Renders visParams.formula (a vegetation index) over window of ds with its
    /// colormap: RGBA interleaved, outPixels() * 4 bytes, nodata transparent.
    std::vector<uint8_t> renderFormulaRgba(GDALDatasetH ds, const std::string &rasterPath,
                                           const RasterWindow &window, const ThumbVisParams &visParams);
}

#endif // VIS_RENDER_H
