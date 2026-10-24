/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "vis_render.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <tuple>

#include "cancellation.h"
#include "exceptions.h"
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

    FormulaPlan planFormula(GDALDatasetH ds, const std::string &formulaId, const BandFilter &filter,
                            const std::string &colormapId, bool withColormap)
    {
        auto &ve = VegetationEngine::instance();
        FormulaPlan plan;
        plan.bandCount = GDALGetRasterCount(ds);
        plan.filter = filter;

        plan.formula = ve.getFormula(formulaId);
        if (!plan.formula)
            throw InvalidArgsException("Unknown formula: " + formulaId);

        if (withColormap)
        {
            const std::string id = colormapId.empty() ? "rdylgn" : colormapId;
            plan.colormap = ve.getColormap(id);
            if (!plan.colormap)
                throw InvalidArgsException("Unknown colormap: " + id);
        }

        plan.nodata = detectBandNodata(ds, plan.bandCount);
        return plan;
    }

    FormulaPlan planFormula(GDALDatasetH ds, const std::string &rasterPath, const std::string &formulaId,
                            const std::string &bandFilter, const std::string &colormapId, bool withColormap)
    {
        const BandFilter filter = !bandFilter.empty()
                                      ? VegetationEngine::parseFilter(bandFilter, GDALGetRasterCount(ds))
                                      : VegetationEngine::instance().autoDetectFilter(rasterPath);
        return planFormula(ds, formulaId, filter, colormapId, withColormap);
    }

    void computeFormulaValues(GDALDatasetH ds, const FormulaPlan &plan, const RasterWindow &window,
                              FormulaBuffers &buffers, NodataMask mask)
    {
        const size_t pixels = window.outPixels();
        buffers.bands.resize(plan.bandCount);
        buffers.ptrs.resize(plan.bandCount);

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

        cancel::GdalProgress progress; // the call is canceled (DQ-15)
        GDALRasterIOExtraArg extra = progress.rasterIOExtraArg();
        for (int b = 0; b < plan.bandCount; b++)
        {
            if (!read[b])
            {
                buffers.ptrs[b] = nullptr;
                continue;
            }
            buffers.bands[b].resize(pixels);
            buffers.ptrs[b] = buffers.bands[b].data();
            if (GDALRasterIOEx(GDALGetRasterBand(ds, b + 1), GF_Read, window.x, window.y, window.xsize, window.ysize,
                               buffers.ptrs[b], window.outWidth, window.outHeight, GDT_Float32, 0, 0, &extra) !=
                CE_None)
            {
                cancel::throwIfCanceled();
                throw GDALException("Cannot read band " + std::to_string(b + 1));
            }
        }

        // Transparent and nodata pixels stay out of the formula (of the bands that were read)
        premaskNodata(buffers.ptrs, pixels, plan.bandCount, plan.nodata, NODATA_SENTINEL);

        buffers.result.resize(pixels);
        VegetationEngine::instance().applyFormula(*plan.formula, plan.filter, buffers.ptrs, buffers.result.data(),
                                                  pixels, NODATA_SENTINEL);
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
                                                   *plan.colormap, range.first, range.second, NODATA_SENTINEL);
        return rgba;
    }

    std::vector<uint8_t> renderFormulaRgba(GDALDatasetH ds, const std::string &rasterPath,
                                           const RasterWindow &window, const ThumbVisParams &visParams)
    {
        const FormulaPlan plan = planFormula(ds, rasterPath, visParams.formula, visParams.bandFilter,
                                             visParams.colormap, true);
        return renderPlannedFormulaRgba(ds, plan, window, visParams.rescale);
    }
}
