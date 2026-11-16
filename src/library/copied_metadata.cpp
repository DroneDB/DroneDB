/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "copied_metadata.h"

#include <algorithm>
#include <cctype>
#include <string_view>

#include "exceptions.h"
#include "gdal_metadata.h"
#include "raster_source.h"

namespace ddb
{
    namespace
    {
        // Whether `name` starts with `prefix`, in any case (STARTS_WITH_CI)
        bool startsWithNoCase(std::string_view name, std::string_view prefix)
        {
            return name.size() >= prefix.size() &&
                   std::equal(prefix.begin(), prefix.end(), name.begin(), [](char a, char b) {
                       return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
                   });
        }

        // The items of a metadata list GTiff writes (CPLParseNameValue), but those `skip` names, in order
        template <typename Skip> CopiedMetadata::Items itemsOf(const std::vector<std::string> &list, Skip skip)
        {
            CopiedMetadata::Items items;
            for (const std::string &item : list)
            {
                if (skip(item))
                    continue;
                if (auto pair = gdalmeta::parseNameValue(item))
                    items.push_back(std::move(*pair));
            }
            return items;
        }

        const std::vector<std::string> &domainItems(const gdalmeta::RasterMetadata &metadata, std::string_view name)
        {
            static const std::vector<std::string> none;
            for (const auto &d : metadata.domains)
                if (d.name == name)
                    return d.items;
            return none;
        }

        const std::vector<std::string> &bandItems(const gdalmeta::RasterMetadata &metadata, int band)
        {
            static const std::vector<std::string> none;
            return band >= 1 && static_cast<size_t>(band) <= metadata.bandItems.size()
                       ? metadata.bandItems[static_cast<size_t>(band - 1)]
                       : none;
        }

        bool isStatistic(const std::string &item) { return item.rfind("STATISTICS_", 0) == 0; }
    } // namespace

    void CopiedMetadata::addTo(json &options) const
    {
        const auto pairs = [](const Items &items) {
            json out = json::array();
            for (const auto &[name, value] : items)
                out.push_back({name, value});
            return out;
        };
        if (!dataset.empty())
            options["metadata"] = pairs(dataset);
        if (!bands.empty())
        {
            json perBand = json::array();
            for (const Items &items : bands)
                perBand.push_back(pairs(items));
            options["bandMetadata"] = std::move(perBand);
        }
        if (!scales.empty())
            options["scales"] = scales;
        if (!offsets.empty())
            options["offsets"] = offsets;
        if (!units.empty())
            options["units"] = units;
        if (!descriptions.empty())
            options["descriptions"] = descriptions;
        if (!xmp.empty())
            options["xmp"] = xmp;
    }

    CopiedMetadata translatedMetadata(const RasterSource &source, const std::vector<int> &bands, bool valuesChanged,
                                      bool wholeExtent)
    {
        const gdalmeta::RasterMetadata metadata = source.metadata();
        bool allBandsInOrder = static_cast<int>(bands.size()) == source.bandCount();
        for (size_t i = 0; i < bands.size() && allBandsInOrder; i++)
            allBandsInOrder = bands[i] == static_cast<int>(i) + 1;

        CopiedMetadata out;
        out.dataset = itemsOf(domainItems(metadata, ""), [&](const std::string &item) {
            return startsWithNoCase(item, "TIFFTAG_MINSAMPLEVALUE=") || startsWithNoCase(item, "TIFFTAG_MAXSAMPLEVALUE=") ||
                   (!wholeExtent && startsWithNoCase(item, "NITF_BLOCKA_")) || startsWithNoCase(item, "CACHE_PATH=") ||
                   (!allBandsInOrder && startsWithNoCase(item, "NODATA_VALUES="));
        });
        const auto &xmp = domainItems(metadata, "xml:XMP");
        if (!xmp.empty())
            out.xmp = xmp.front();

        // GDAL leaves the statistics out when the values or the extent change (bFilterOutStatsMetadata), and
        // copies the unit only with them
        const bool keepStatistics = !valuesChanged && wholeExtent;
        for (const int b : bands)
        {
            out.bands.push_back(itemsOf(bandItems(metadata, b), [&](const std::string &item) {
                return !keepStatistics && isStatistic(item);
            }));
            out.scales.push_back(source.scale(b).value_or(1.0));
            out.offsets.push_back(source.offset(b).value_or(0.0));
            out.units.push_back(keepStatistics ? source.unit(b) : std::string());
            out.descriptions.push_back(source.description(b));
        }
        return out;
    }

    CopiedMetadata warpedMetadata(const RasterSource &source, const std::vector<int> &bands, int outputBands,
                                  bool addedAlpha)
    {
        const gdalmeta::RasterMetadata metadata = source.metadata();
        CopiedMetadata out;
        out.dataset = itemsOf(domainItems(metadata, ""), [&](const std::string &item) {
            return startsWithNoCase(item, "CACHE_PATH=") || (addedAlpha && startsWithNoCase(item, "NODATA_VALUES="));
        });

        // The scale and offset of every warped band (gdalwarp: nBandsToCopy, from the band each one warps)
        const int warped = std::min(outputBands - (addedAlpha ? 1 : 0), source.bandCount());
        out.scales.assign(static_cast<size_t>(outputBands), 1.0);
        out.offsets.assign(static_cast<size_t>(outputBands), 0.0);
        for (int i = 0; i < warped; i++)
        {
            if (!bands.empty() && static_cast<size_t>(i) >= bands.size())
                throw InvalidArgsException("warpedMetadata: " + std::to_string(outputBands) + " output bands for " +
                                           std::to_string(bands.size()) + " warped");
            const int b = bands.empty() ? i + 1 : bands[static_cast<size_t>(i)];
            out.scales[static_cast<size_t>(i)] = source.scale(b).value_or(1.0);
            out.offsets[static_cast<size_t>(i)] = source.offset(b).value_or(0.0);
        }

        // The other information of each band only when the band counts match, band i from source band i
        if (outputBands == source.bandCount())
            for (int b = 1; b <= outputBands; b++)
            {
                out.bands.push_back(itemsOf(bandItems(metadata, b), isStatistic));
                out.units.push_back(source.unit(b));
                out.descriptions.push_back(source.description(b));
            }
        return out;
    }

} // namespace ddb
