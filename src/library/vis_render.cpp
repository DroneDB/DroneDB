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

    std::vector<uint8_t> renderFormulaRgba(GDALDatasetH ds, const std::string &rasterPath,
                                           const RasterWindow &window, const ThumbVisParams &visParams)
    {
        auto &ve = VegetationEngine::instance();
        const int bandCount = GDALGetRasterCount(ds);
        const size_t pixels = window.outPixels();

        const auto *formula = ve.getFormula(visParams.formula);
        if (!formula)
            throw InvalidArgsException("Unknown formula: " + visParams.formula);

        const std::string colormapId = visParams.colormap.empty() ? "rdylgn" : visParams.colormap;
        const auto *colormap = ve.getColormap(colormapId);
        if (!colormap)
            throw InvalidArgsException("Unknown colormap: " + colormapId);

        const BandFilter filter = !visParams.bandFilter.empty()
                                      ? VegetationEngine::parseFilter(visParams.bandFilter, bandCount)
                                      : ve.autoDetectFilter(rasterPath);

        // Read every band as float
        std::vector<std::vector<float>> storage(bandCount);
        std::vector<float *> bands(bandCount);
        const auto nodataInfo = detectBandNodata(ds, bandCount);
        cancel::GdalProgress progress; // the call is canceled (DQ-15)
        GDALRasterIOExtraArg extra = progress.rasterIOExtraArg();
        for (int b = 0; b < bandCount; b++)
        {
            storage[b].resize(pixels);
            bands[b] = storage[b].data();
            if (GDALRasterIOEx(GDALGetRasterBand(ds, b + 1), GF_Read, window.x, window.y, window.xsize, window.ysize,
                               bands[b], window.outWidth, window.outHeight, GDT_Float32, 0, 0, &extra) != CE_None)
            {
                cancel::throwIfCanceled();
                throw GDALException("Cannot read band " + std::to_string(b + 1));
            }
        }

        // Transparent and nodata pixels stay out of the formula
        const float nodata = NODATA_SENTINEL;
        premaskNodata(bands, pixels, bandCount, nodataInfo, nodata);

        std::vector<float> result(pixels);
        ve.applyFormula(*formula, filter, bands, result.data(), pixels, nodata);

        // Display range: explicit, the formula's own, or p2-p98 of the rendered values
        float rMin = 0, rMax = 1;
        if (!visParams.rescale.empty())
        {
            std::tie(rMin, rMax) = parseRescaleRange(visParams.rescale);
        }
        else if (formula->hasRange && formula->rangeMin != formula->rangeMax)
        {
            rMin = static_cast<float>(formula->rangeMin);
            rMax = static_cast<float>(formula->rangeMax);
        }
        else
        {
            std::vector<float> valid;
            valid.reserve(pixels);
            for (const float v : result)
                if (v != nodata && !std::isnan(v))
                    valid.push_back(v);
            if (!valid.empty())
            {
                std::sort(valid.begin(), valid.end());
                rMin = valid[static_cast<size_t>(valid.size() * 0.02)];
                rMax = valid[std::min(valid.size() - 1, static_cast<size_t>(valid.size() * 0.98))];
            }
        }

        std::vector<uint8_t> rgba(pixels * 4);
        ve.applyColormap(result.data(), rgba.data(), pixels, *colormap, rMin, rMax, nodata);
        return rgba;
    }
}
