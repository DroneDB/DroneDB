/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "vis_render.h"
#include <cstdio>

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <tuple>

#include "cancellation.h"
#include "exceptions.h"
#include "logger.h"
#include "raster_utils.h"
#include "sensorprofile.h"
#include "vegetation.h"

namespace ddb
{
    std::pair<float, float> parseRescaleRange(const std::string &rescale)
    {
        const auto commaPos = rescale.find(',');
        const std::string minStr = commaPos == std::string::npos ? "" : rescale.substr(0, commaPos);
        const std::string maxStr = commaPos == std::string::npos ? "" : rescale.substr(commaPos + 1);
        if (minStr.empty() || maxStr.empty())
            throw InvalidArgsException("Invalid rescale format: " + rescale + ". Expected format: min,max");
        try
        {
            return {std::stof(minStr), std::stof(maxStr)};
        }
        catch (const std::exception &)
        {
            throw InvalidArgsException("Invalid rescale values: " + rescale + ". Expected numeric format: min,max");
        }
    }

    std::string convertRescale(const std::string &rescale, TemperatureUnit from, TemperatureUnit to)
    {
        parseRescaleRange(rescale); // the format is checked once, with its message
        const auto commaPos = rescale.find(',');
        const TemperatureConversion conversion = temperatureConversion(from, to);
        double lo, hi;
        try
        {
            lo = conversion.apply(std::stod(rescale.substr(0, commaPos)));
            hi = conversion.apply(std::stod(rescale.substr(commaPos + 1)));
        }
        catch (const std::exception &)
        {
            throw InvalidArgsException("Invalid rescale values: " + rescale + ". Expected numeric format: min,max");
        }
        char text[64];
        std::snprintf(text, sizeof(text), "%.9g,%.9g", lo, hi);
        return text;
    }

    std::vector<int> resolveVisBands(const std::string &rasterPath, const ThumbVisParams &visParams, int maxBand)
    {
        std::vector<int> bands;
        if (!visParams.bands.empty())
        {
            std::istringstream ss(visParams.bands);
            std::string token;
            while (std::getline(ss, token, ','))
            {
                int b;
                try
                {
                    b = std::stoi(token);
                }
                catch (const std::exception &)
                {
                    throw InvalidArgsException("Invalid band value: '" + token + "'");
                }
                if (b < 1 || b > maxBand)
                    throw InvalidArgsException("Band index " + std::to_string(b) + " out of range (1-" +
                                               std::to_string(maxBand) + ")");
                bands.push_back(b);
            }
        }
        else if (!visParams.preset.empty())
        {
            const auto mapping = SensorProfileManager::instance().getBandMappingForPreset(rasterPath, visParams.preset);
            bands = {mapping.r, mapping.g, mapping.b};
        }
        else
        {
            const auto detection = SensorProfileManager::instance().detectSensor(rasterPath);
            if (detection.detected)
                bands = {detection.defaultBandMapping.r, detection.defaultBandMapping.g,
                         detection.defaultBandMapping.b};
        }
        return bands;
    }

    std::pair<double, double> stretchRange(const BandStatistics &s)
    {
        double sMin = std::max(s.mean - 2.33 * s.stdDev, s.min);
        double sMax = std::min(s.mean + 2.33 * s.stdDev, s.max);
        if (sMin >= sMax)
        {
            sMin = s.min;
            sMax = s.max;
        }
        if (sMin >= sMax)
            sMax = sMin + 1.0;
        return {sMin, sMax};
    }

    int dataBandCount(GDALDatasetH ds)
    {
        const GDALRasterBandH raster = GDALGetRasterBand(ds, 1);
        const GDALRasterBandH alphaBand = GDALGetMaskBand(raster);
        const int bandsCount = GDALGetRasterCount(ds);
        const GDALRasterBandH lastBand = GDALGetRasterBand(ds, bandsCount);

        if (GDALGetMaskFlags(alphaBand) & GMF_ALPHA || bandsCount == 4 ||
            bandsCount == 2 || GDALGetRasterColorInterpretation(lastBand) == GCI_AlphaBand)
        {
            return bandsCount - 1;
        }

        return bandsCount;
    }

    std::vector<int> defaultDisplayBands(GDALDatasetH ds)
    {
        return dataBandCount(ds) >= 3 ? std::vector<int>{1, 2, 3} : std::vector<int>{1};
    }

    std::pair<double, double> displayRange(const std::string &path, GDALDatasetH ds, const std::vector<int> &bands)
    {
        double lo = std::numeric_limits<double>::max();
        double hi = std::numeric_limits<double>::lowest();
        for (const int b : bands)
        {
            const BandStatistics s = RasterStatsProvider::instance().get(path, ds, b);
            lo = std::min(lo, s.min);
            hi = std::max(hi, s.max);
        }
        // Avoid divide by zero
        if (lo == hi)
            hi += 0.1;
        // Can still happen according to GDAL for very large values
        if (lo == hi)
            throw GDALException("Cannot scale values due to source min/max being equal");
        LOGD << "Min: " << lo << " | Max: " << hi;
        return {lo, hi};
    }

    std::vector<std::pair<double, double>> defaultStretch(const std::string &path, GDALDatasetH ds,
                                                          const std::vector<int> &bands,
                                                          const std::optional<double> &nodata, StretchSource source)
    {
        auto &provider = RasterStatsProvider::instance();
        // The statistics of the bands: exact, or those of a thumbnail (the bands in one bounded read, D-041)
        std::vector<BandStatistics> stats;
        if (source == StretchSource::Bounded)
            stats = provider.getStretchStatistics(path, ds, bands, nodata);
        else
            for (const int b : bands)
                stats.push_back(provider.get(path, ds, b, nodata));

        std::vector<std::pair<double, double>> ranges;
        if (bands.size() == 1)
        {
            const BandStatistics &s = stats[0];
            double lo = s.min, hi = s.max;
            // Avoid divide by zero
            if (lo == hi)
                hi += 0.1;
            if (lo == hi)
                throw GDALException("Cannot scale values due to source min/max being equal");
            ranges.emplace_back(lo, hi);
            return ranges;
        }
        for (const BandStatistics &s : stats)
            ranges.push_back(stretchRange(s));
        return ranges;
    }

    RasterDataKind rasterDataKind(GDALDatasetH ds)
    {
        RasterDataKind kind;
        kind.floatingPoint = true;
        const int bandCount = GDALGetRasterCount(ds);
        for (int b = 1; b <= bandCount; b++)
        {
            GDALRasterBandH band = GDALGetRasterBand(ds, b);
            if (GDALGetRasterColorInterpretation(band) == GCI_AlphaBand)
                continue;
            kind.bandCount++;
            const GDALDataType type = GDALGetRasterDataType(band);
            if (kind.floatingPoint && (kind.typeName.empty() || !GDALDataTypeIsFloating(type)))
                kind.typeName = GDALGetDataTypeName(type);
            if (!GDALDataTypeIsFloating(type))
                kind.floatingPoint = false;
        }
        return kind;
    }

    FormulaPlan planFormula(GDALDatasetH ds, const std::string &formulaId, const BandFilter &filter,
                            const std::string &colormapId, bool withColormap)
    {
        auto &ve = VegetationEngine::instance();
        FormulaPlan plan;
        plan.bandCount = GDALGetRasterCount(ds);
        plan.filter = filter;

        // A formula that does not exist, or that this raster cannot compute, is refused: the same
        // call can never succeed, so every C export returns it as DDBERR_INVALIDARGS
        // (RefusedArgumentException), with the reason alone
        plan.formula = ve.getFormula(formulaId);
        if (!plan.formula)
            throw RefusedArgumentException("Unknown formula: " + formulaId);

        // The rule of what a raster can compute (the lists of formulas ask its base,
        // unavailableReason): a band the filter does not map ("Requires bands: RN" for NDVI over
        // an RGB raster; not for the thermal formulas, which read the first band), then the data
        const std::string refusal = VegetationEngine::renderRefusal(*plan.formula, filter, rasterDataKind(ds));
        if (!refusal.empty())
            throw RefusedArgumentException(refusal);

        if (withColormap)
        {
            const std::string id = colormapId.empty() ? "rdylgn" : colormapId;
            plan.colormap = ve.getColormap(id);
            if (!plan.colormap)
                throw InvalidArgsException("Unknown colormap: " + id);
        }

        plan.nodata = detectBandNodata(ds, plan.bandCount);
        if (plan.bandCount >= 1)
        {
            // The thermal formulas convert from the unit of the first band to their own (D-022)
            const char *unitType = GDALGetRasterUnitType(GDALGetRasterBand(ds, 1));
            plan.bandUnit = bandTemperatureUnit(unitType != nullptr ? unitType : "");
        }
        return plan;
    }

    FormulaPlan planFormula(GDALDatasetH ds, const std::string &rasterPath, const std::string &formulaId,
                            const std::string &bandFilter, const std::string &colormapId, bool withColormap,
                            std::optional<bool> thermalTags)
    {
        const BandFilter filter = !bandFilter.empty()
                                      ? VegetationEngine::parseFilter(bandFilter, GDALGetRasterCount(ds))
                                      : VegetationEngine::instance().autoDetectFilter(rasterPath, thermalTags);
        return planFormula(ds, formulaId, filter, colormapId, withColormap);
    }

    namespace
    {
        // Reads window of the bands of ds that `read` marks (the others get a null entry of buffers.ptrs) as
        // float, masks the transparent and nodata pixels with the sentinel and gives the physical values of a
        // band: raw * scale + offset (FR-RAS-04). `nodata` is that of the raster (detectBandNodata)
        void readPhysicalBands(GDALDatasetH ds, const RasterBandNodata &nodata, int bandCount,
                               const std::vector<bool> &read, const RasterWindow &window, FormulaBuffers &buffers)
        {
            const size_t pixels = window.outPixels();
            buffers.bands.resize(bandCount);
            buffers.ptrs.resize(bandCount);

            cancel::GdalProgress progress; // the call is canceled
            GDALRasterIOExtraArg extra = progress.rasterIOExtraArg();
            // Reads the window of a band into out, as type
            const auto readWindow = [&](GDALRasterBandH band, void *out, GDALDataType type, const std::string &what) {
                if (GDALRasterIOEx(band, GF_Read, window.x, window.y, window.xsize, window.ysize, out,
                                   window.outWidth, window.outHeight, type, 0, 0, &extra) != CE_None)
                {
                    cancel::throwIfCanceled();
                    throw GDALException("Cannot read " + what);
                }
            };
            for (int b = 0; b < bandCount; b++)
            {
                if (!read[b])
                {
                    buffers.ptrs[b] = nullptr;
                    continue;
                }
                buffers.bands[b].resize(pixels);
                buffers.ptrs[b] = buffers.bands[b].data();
                readWindow(GDALGetRasterBand(ds, b + 1), buffers.ptrs[b], GDT_Float32, "band " + std::to_string(b + 1));
            }

            // The dataset mask, when the raster keeps its transparency there instead of in an alpha band
            const uint8_t *datasetMask = nullptr;
            if (nodata.datasetMask)
            {
                buffers.mask.resize(pixels);
                readWindow(GDALGetMaskBand(GDALGetRasterBand(ds, 1)), buffers.mask.data(), GDT_Byte,
                           "the dataset mask");
                datasetMask = buffers.mask.data();
            }

            // Transparent and nodata pixels stay out of the values (of the bands that were read)
            premaskNodata(buffers.ptrs, pixels, bandCount, nodata, NODATA_SENTINEL, datasetMask);

            // The values are the physical ones: raw * scale + offset. The pixels with no data were compared with
            // the raw samples above and keep the sentinel
            for (int b = 0; b < bandCount; b++)
            {
                if (buffers.ptrs[b] == nullptr)
                    continue;
                const GDALRasterBandH hBand = GDALGetRasterBand(ds, b + 1);
                int hasScale = FALSE, hasOffset = FALSE;
                const double scale = GDALGetRasterScale(hBand, &hasScale);
                const double offset = GDALGetRasterOffset(hBand, &hasOffset);
                if (!((hasScale && scale != 1.0) || (hasOffset && offset != 0.0)))
                    continue;
                const double m = hasScale ? scale : 1.0;
                const double c = hasOffset ? offset : 0.0;
                float *values = buffers.ptrs[b];
                for (size_t i = 0; i < pixels; i++)
                    if (values[i] != NODATA_SENTINEL)
                        values[i] = static_cast<float>(values[i] * m + c);
            }
        }
    }

    void computeFormulaValues(GDALDatasetH ds, const FormulaPlan &plan, const RasterWindow &window,
                              FormulaBuffers &buffers, NodataMask mask)
    {
        const size_t pixels = window.outPixels();

        // The bands to read: all of them, or those the formula reads and the alpha band
        std::vector<bool> read(plan.bandCount, true);
        if (mask == NodataMask::BandsRead)
        {
            std::fill(read.begin(), read.end(), false);
            for (const int b : VegetationEngine::bandsRead(*plan.formula, plan.filter))
                if (b < plan.bandCount)
                    read[b] = true;
            if (plan.nodata.alphaBandIdx >= 0)
                read[plan.nodata.alphaBandIdx] = true;
        }

        readPhysicalBands(ds, plan.nodata, plan.bandCount, read, window, buffers);

        buffers.result.resize(pixels);
        VegetationEngine::instance().applyFormula(*plan.formula, plan.filter, buffers.ptrs, buffers.result.data(),
                                                  pixels, NODATA_SENTINEL, plan.bandUnit);
    }

    void computeBandValues(GDALDatasetH ds, const RasterBandNodata &nodata, int band, const RasterWindow &window,
                           FormulaBuffers &buffers)
    {
        const int bandCount = GDALGetRasterCount(ds);
        if (band < 1 || band > bandCount)
            throw InvalidArgsException("Band index " + std::to_string(band) + " out of range (1-" +
                                       std::to_string(bandCount) + ")");

        // The band and the alpha band, which says "no data here" for every band
        std::vector<bool> read(bandCount, false);
        read[band - 1] = true;
        if (nodata.alphaBandIdx >= 0)
            read[nodata.alphaBandIdx] = true;

        readPhysicalBands(ds, nodata, bandCount, read, window, buffers);
        buffers.result.swap(buffers.bands[band - 1]);
    }

    std::optional<std::pair<float, float>> fixedDisplayRange(const std::string &rescale,
                                                             const VegetationFormula &formula)
    {
        if (!rescale.empty())
            return parseRescaleRange(rescale);
        if (formula.hasRange && formula.rangeMin != formula.rangeMax)
            return std::make_pair(static_cast<float>(formula.rangeMin), static_cast<float>(formula.rangeMax));
        return std::nullopt;
    }

    std::pair<float, float> percentileDisplayRange(std::vector<float> &values)
    {
        if (values.empty())
            return {0.0f, 1.0f};
        std::sort(values.begin(), values.end());
        return {values[static_cast<size_t>(values.size() * 0.02)],
                values[std::min(values.size() - 1, static_cast<size_t>(values.size() * 0.98))]};
    }

    std::vector<uint8_t> renderPlannedFormulaRgba(GDALDatasetH ds, const FormulaPlan &plan,
                                                  const RasterWindow &window, const std::string &rescale,
                                                  NodataMask mask)
    {
        if (plan.colormap == nullptr)
            throw InvalidArgsException("renderPlannedFormulaRgba: the plan has no colormap");
        FormulaBuffers buffers;
        computeFormulaValues(ds, plan, window, buffers, mask);

        // Display range: explicit, the formula's own, or p2-p98 of the rendered values
        std::pair<float, float> range;
        if (const auto fixed = fixedDisplayRange(rescale, *plan.formula))
        {
            range = *fixed;
        }
        else
        {
            std::vector<float> valid;
            valid.reserve(buffers.result.size());
            for (const float v : buffers.result)
                if (isFormulaValue(v))
                    valid.push_back(v);
            range = percentileDisplayRange(valid);
        }

        std::vector<uint8_t> rgba(window.outPixels() * 4);
        VegetationEngine::instance().applyColormap(buffers.result.data(), rgba.data(), window.outPixels(),
                                                   *plan.colormap, {range.first, range.second, NODATA_SENTINEL});
        return rgba;
    }

    std::vector<uint8_t> renderFormulaRgba(GDALDatasetH ds, const std::string &rasterPath,
                                           const RasterWindow &window, const ThumbVisParams &visParams,
                                           std::optional<bool> thermalTags)
    {
        const FormulaPlan plan = planFormula(ds, rasterPath, visParams.formula, visParams.bandFilter,
                                             visParams.colormap, true, thermalTags);
        return renderPlannedFormulaRgba(ds, plan, window, visParams.rescale);
    }

    int styledBand(GDALDatasetH ds, const std::string &colormap, const std::vector<int> &chosen)
    {
        if (colormap.empty())
            return 0;
        if (chosen.empty())
            return dataBandCount(ds) == 1 ? 1 : 0;
        for (const int b : chosen)
            if (b != chosen.front())
                return 0;
        return chosen.front();
    }

    BandStylePlan planBandStyle(const std::string &path, GDALDatasetH ds, int band, const std::string &colormapId,
                                const std::string &rescale, const std::optional<double> &nodata,
                                StretchSource source)
    {
        BandStylePlan plan;
        plan.band = band;
        plan.colormap = VegetationEngine::instance().getColormap(colormapId);
        if (plan.colormap == nullptr)
            throw InvalidArgsException("Unknown colormap: " + colormapId);

        if (!rescale.empty())
        {
            plan.range = parseRescaleRange(rescale);
            return plan;
        }

        const auto [lo, hi] = defaultStretch(path, ds, {band}, nodata, source)[0];
        // The stretch is in the raw values of the band
        int hasScale = FALSE, hasOffset = FALSE;
        const GDALRasterBandH hBand = GDALGetRasterBand(ds, band);
        const double scale = GDALGetRasterScale(hBand, &hasScale);
        const double offset = GDALGetRasterOffset(hBand, &hasOffset);
        const double m = hasScale ? scale : 1.0, c = hasOffset ? offset : 0.0;
        plan.range = {static_cast<float>(std::min(lo * m + c, hi * m + c)),
                      static_cast<float>(std::max(lo * m + c, hi * m + c))};
        return plan;
    }

    std::optional<BandStylePlan> planVisBandStyle(const std::string &path, GDALDatasetH ds,
                                                  const ThumbVisParams &visParams, int maxBand,
                                                  StretchSource source)
    {
        if (!visParams.formula.empty() || visParams.colormap.empty())
            return std::nullopt;
        // Nothing chosen is not the bands of the detected sensor here: a raster of one data band is that band
        const std::vector<int> chosen = visParams.bands.empty() && visParams.preset.empty()
                                            ? std::vector<int>{}
                                            : resolveVisBands(path, visParams, maxBand);
        const int band = styledBand(ds, visParams.colormap, chosen);
        if (band == 0)
            return std::nullopt;
        return planBandStyle(path, ds, band, visParams.colormap, visParams.rescale, visParams.nodata, source);
    }

    std::vector<uint8_t> renderBandStyleRgba(GDALDatasetH ds, const BandStylePlan &plan, const RasterWindow &window)
    {
        FormulaBuffers buffers;
        computeBandValues(ds, detectBandNodata(ds, GDALGetRasterCount(ds)), plan.band, window, buffers);

        std::vector<uint8_t> rgba(window.outPixels() * 4);
        VegetationEngine::instance().applyColormap(buffers.result.data(), rgba.data(), window.outPixels(),
                                                   *plan.colormap,
                                                   {plan.range.first, plan.range.second, NODATA_SENTINEL});
        return rgba;
    }
}
