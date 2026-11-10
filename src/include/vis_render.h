/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef VIS_RENDER_H
#define VIS_RENDER_H

#include <algorithm>
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

    /// The rescale string "min,max" given in the unit `from`, as a rescale string in the unit `to` (D-022): the
    /// range of a thermal formula that a client set in another unit keeps its temperatures. Throws
    /// InvalidArgsException for a rescale that is not "min,max" of numbers.
    std::string convertRescale(const std::string &rescale, TemperatureUnit from, TemperatureUnit to);

    /// Bands to show as R, G, B for visParams: the explicit "bands" list (each checked
    /// against 1..maxBand), the preset's mapping, or the default mapping of the detected
    /// sensor. Empty when nothing selects bands (show the first bands).
    std::vector<int> resolveVisBands(const std::string &rasterPath, const ThumbVisParams &visParams,
                                     int maxBand);

    /// Display range of a band: mean ± 2.33 σ (≈ p2-p98) clipped to [min, max]
    std::pair<double, double> stretchRange(const BandStatistics &s);

    /// The bands of ds that hold data: all but a last band that is alpha (by its color
    /// interpretation or the mask of the raster, or the 2nd of 2 and the 4th of 4 bands)
    int dataBandCount(GDALDatasetH ds);

    /// How a raster shows without visualisation parameters: the map tiles and the WMS map draw
    /// its first three data bands as RGB (its first band as grey when it has fewer), and when they
    /// are not 8-bit, stretch them together over the range of their statistics (displayRange)
    std::vector<int> defaultDisplayBands(GDALDatasetH ds);

    /// The range that bands of the raster at path (ds, open) are stretched over to show them: the
    /// lowest minimum to the highest maximum of their statistics, one range for all so their
    /// colours keep their balance. Throws GDALException when it is empty.
    std::pair<double, double> displayRange(const std::string &path, GDALDatasetH ds, const std::vector<int> &bands);

    /// Where the default stretch of a render reads the statistics of its bands (D-041)
    enum class StretchSource
    {
        // The exact statistics (RasterStatsProvider::get): the map tiles, the export, the region render
        Exact,
        // The statistics of a thumbnail (RasterStatsProvider::getStretchStatistics): the exact ones when they are
        // known or the band is small, else a bounded sample, so a thumbnail fits in the limits of the worker
        Bounded
    };

    /// The default stretch of the non-Byte bands of the raster at path (ds, open), one range per band, in
    /// the raw values of the bands (D-041, FR-RAS-21): a single band (a DEM, a thermal raster) over its
    /// minimum to its maximum, several bands each over the mean +- 2.33 standard deviations of the band
    /// (stretchRange), so a 16-bit multispectral ortho is readable. `nodata` is the override of the
    /// NoData value (FR-RAS-31); `source` says which statistics it reads. Throws GDALException when a band
    /// has no pixel with a value.
    std::vector<std::pair<double, double>> defaultStretch(const std::string &path, GDALDatasetH ds,
                                                          const std::vector<int> &bands,
                                                          const std::optional<double> &nodata = std::nullopt,
                                                          StretchSource source = StretchSource::Exact);

    /// v stretched from [lo, hi] to 0-255, clipped
    inline uint8_t stretchToByte(double v, double lo, double hi)
    {
        const double c = std::max(lo, std::min(hi, v));
        return static_cast<uint8_t>(255.0 * (c - lo) / (hi - lo));
    }

    /// The type of the data bands of ds (the alpha band does not count): floating point when every
    /// one is Float32 or Float64. What decides whether a formula that needs reflectance can be
    /// computed (VegetationEngine::dataRefusal).
    RasterDataKind rasterDataKind(GDALDatasetH ds);

    /// What the render of a formula over one raster decides once: the formula, the band filter
    /// that maps R, G, B, N, Re and T to bands, the colormap and the nodata of every band. The
    /// thumbnails, tiles, statistics and the export of a formula all start from one.
    struct FormulaPlan
    {
        const VegetationFormula *formula = nullptr;
        BandFilter filter;
        const Colormap *colormap = nullptr; // null when the plan was made without one
        RasterBandNodata nodata;            // per band, and the alpha band or the dataset mask
        int bandCount = 0;
        /// The unit of the first band (its unit type; degrees Celsius when it names none): what a thermal
        /// formula (CELSIUS, KELVIN, FAHRENHEIT) converts from (D-022)
        TemperatureUnit bandUnit = TemperatureUnit::Celsius;
    };

    /// Resolves formulaId, the band filter (bandFilter, "" = detected from the sensor profile of
    /// rasterPath, VegetationEngine::autoDetectFilter with thermalTags) and, with withColormap, the
    /// colormap (colormapId, "" = "rdylgn"). Throws InvalidArgsException for an unknown colormap,
    /// and RefusedArgumentException (DDBERR_INVALIDARGS in every C export) for an unknown formula,
    /// for a formula whose bands the filter does not map ("Requires bands: RN" for NDVI over an
    /// RGB raster; not the thermal formulas, which read the first band), and for a formula that
    /// needs reflectance over a raster whose data is not floating point (SAVI, OSAVI, EVI over
    /// UInt16): the rule of VegetationEngine::renderRefusal. Every render of a formula plans it
    /// here, so none can skip the rule.
    FormulaPlan planFormula(GDALDatasetH ds, const std::string &rasterPath, const std::string &formulaId,
                            const std::string &bandFilter, const std::string &colormapId, bool withColormap,
                            std::optional<bool> thermalTags = std::nullopt);

    /// The same with the band filter already resolved
    FormulaPlan planFormula(GDALDatasetH ds, const std::string &formulaId, const BandFilter &filter,
                            const std::string &colormapId, bool withColormap);

    /// Which bands decide that a pixel has no value, when a formula is computed
    enum class NodataMask
    {
        /// A pixel with no data (nodata, NaN) in ANY band of the raster, or transparent in the alpha
        /// band or the dataset mask, is left out. The rule of the thumbnails, the tiles and the
        /// formula renders: the same pixels are blank whichever formula is shown.
        AnyBand,
        /// Only the bands the formula reads (and the alpha band or the dataset mask, which say "no
        /// data here" for every band) decide. A pixel whose only gap is in a band the formula does
        /// not read is computed, and the bands the formula does not read are not even read. The rule of the
        /// vegetation-index map layer. Not for the thermal formulas (CELSIUS, KELVIN, FAHRENHEIT), which read
        /// band 0 whatever the band filter says.
        BandsRead
    };

    /// The buffers a formula computation reuses from window to window
    struct FormulaBuffers
    {
        std::vector<std::vector<float>> bands; // one per band
        std::vector<float *> ptrs;
        std::vector<float> result;
        std::vector<uint8_t> mask; // the window of the dataset mask, when the raster has one
    };

    /// Reads window of every band as float, masks the transparent and nodata pixels and applies
    /// the formula: buffers.result holds window.outPixels() values, NODATA_SENTINEL where a pixel
    /// has none, the pixels that mask being those of `mask`. With BandsRead the bands that are not
    /// read have a null entry in buffers.ptrs. The read stops with the call that is canceled.
    void computeFormulaValues(GDALDatasetH ds, const FormulaPlan &plan, const RasterWindow &window,
                              FormulaBuffers &buffers, NodataMask mask = NodataMask::AnyBand);

    /// Reads window of band `band` (1-based) of ds as float, the physical values (raw * scale + offset) in
    /// buffers.result, NODATA_SENTINEL where a pixel has none: the pixels that are nodata in the band or
    /// transparent in the alpha band or the dataset mask (`nodata` is detectBandNodata of ds). The values that a
    /// band shows with a colormap and no formula. The read stops with the call that is canceled.
    void computeBandValues(GDALDatasetH ds, const RasterBandNodata &nodata, int band, const RasterWindow &window,
                           FormulaBuffers &buffers);

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
    /// colormap: RGBA interleaved, outPixels() * 4 bytes, nodata transparent. thermalTags as
    /// planFormula takes it.
    std::vector<uint8_t> renderFormulaRgba(GDALDatasetH ds, const std::string &rasterPath,
                                           const RasterWindow &window, const ThumbVisParams &visParams,
                                           std::optional<bool> thermalTags = std::nullopt);

    /// One band painted with a colormap and no formula (FR-RAS-01, FR-RAS-26): what the map tiles, the
    /// thumbnails and the export draw of a band that the visualisation gives a colormap and no formula
    struct BandStylePlan
    {
        int band = 0;                       // 1-based
        const Colormap *colormap = nullptr; // never null in a plan
        std::pair<float, float> range;      // the physical values painted from the first to the last colour
    };

    /// The one band that `colormap` paints without a formula, or 0 when there is none: it needs a colormap, and
    /// exactly one band, as `chosen` (the bands that a bands list or a preset choose, empty when neither is given)
    /// names it (the same band three times is one), or the first band of a raster of one data band when nothing
    /// is chosen (the alpha band is not a data band: dataBandCount). A raster of several data bands with no
    /// choice, or a choice of several bands, is not one band: it keeps its bands and the colormap is ignored.
    int styledBand(GDALDatasetH ds, const std::string &colormap, const std::vector<int> &chosen);

    /// The plan of band `band` of the raster at path (ds, open) painted with colormapId (InvalidArgsException
    /// when it does not exist), over the "min,max" rescale, else over the default stretch of the band
    /// (defaultStretch: its minimum to its maximum, from the statistics of the whole raster with the NoData
    /// override `nodata`), both in the physical values of the band (raw * scale + offset), as a formula is.
    /// A band with no pixel that has a value (all NoData or masked) has no range: the plan has the nominal range
    /// 0 to 1 and its render is all transparent. A Byte band is stretched as any other (over its exact minimum to
    /// maximum, which a plain Byte tile does not read). `source` says which statistics the default stretch reads: the
    /// exact ones for a tile and an export, the bounded ones of D-041 for a thumbnail.
    BandStylePlan planBandStyle(const std::string &path, GDALDatasetH ds, int band, const std::string &colormapId,
                                const std::string &rescale, const std::optional<double> &nodata,
                                StretchSource source = StretchSource::Exact);

    /// The plan of visParams for the raster at path (ds, open, whose statistics and scale the stretch reads)
    /// when they paint one band with a colormap and no formula (styledBand, with the bands or the preset
    /// resolved by resolveVisBands against maxBand), else none. `source` as in planBandStyle.
    std::optional<BandStylePlan> planVisBandStyle(const std::string &path, GDALDatasetH ds,
                                                  const ThumbVisParams &visParams, int maxBand,
                                                  StretchSource source = StretchSource::Exact);

    /// Renders the band of the plan over window of ds with its colormap: RGBA interleaved, outPixels() * 4
    /// bytes, a pixel with no data (computeBandValues) transparent
    std::vector<uint8_t> renderBandStyleRgba(GDALDatasetH ds, const BandStylePlan &plan, const RasterWindow &window);
}

#endif // VIS_RENDER_H
