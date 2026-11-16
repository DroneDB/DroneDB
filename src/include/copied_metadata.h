/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef COPIED_METADATA_H
#define COPIED_METADATA_H

#include <string>
#include <utility>
#include <vector>

#include "json.h"

namespace ddb
{
    class RasterSource;

    // The metadata a raster libddb writes takes from its source, as GDAL's tools copy it: the export of
    // bands as gdal_translate copies it, the GeoTIFF of a region render as gdalwarp copies it. ddb-tilers
    // writes it where GDAL's GTiff driver writes it (ddbtWriteRaster), so GDAL reads it back from the
    // output as it read it from the output of the tool.
    struct CopiedMetadata
    {
        using Items = std::vector<std::pair<std::string, std::string>>;
        Items dataset;                         // the default-domain items of the dataset, in order
        std::vector<Items> bands;              // the items of each output band (empty: none)
        std::vector<double> scales, offsets;   // of each output band (empty: none; 1 and 0 write nothing)
        std::vector<std::string> units;        // of each output band, "" for none (empty: none)
        std::vector<std::string> descriptions; // of each output band, "" for none (empty: none)
        std::string xmp;                       // the XMP packet, "" for none

        // Adds the options of ddbtWriteRaster that write it to `options`
        void addTo(json &options) const;
    };

    // What gdal_translate -ot <type> [-srcwin] -b <band>... [-scale ...] copies from `source` to its output, whose
    // bands are the source bands `bands` (1-based, in their order): the items of the dataset but
    // TIFFTAG_MINSAMPLEVALUE and TIFFTAG_MAXSAMPLEVALUE (the type is set), CACHE_PATH, NITF_BLOCKA_* when
    // the output is not the whole extent and NODATA_VALUES unless every band is copied in its order; the XMP
    // packet; for each band its items (but STATISTICS_* when `valuesChanged`, a scale applied, or the output
    // is not the whole extent), its scale and offset, its description, and its unit when no statistic was
    // left out (CopyBandInfo).
    CopiedMetadata translatedMetadata(const RasterSource &source, const std::vector<int> &bands, bool valuesChanged,
                                      bool wholeExtent);

    // What gdalwarp copies from `source` (its only source) to its output of `outputBands` bands: the source
    // bands `bands` warped (1-based; empty: every band), then the alpha band it adds when `addedAlpha`. The
    // items of the dataset but CACHE_PATH, and NODATA_VALUES when it adds an alpha band; the scale and offset
    // of each warped band; when the output has as many bands as the source, the items of each band (but
    // STATISTICS_*), its description and its unit, band by band in their order (ProcessMetadata). No XMP.
    CopiedMetadata warpedMetadata(const RasterSource &source, const std::vector<int> &bands, int outputBands,
                                  bool addedAlpha);

} // namespace ddb

#endif // COPIED_METADATA_H
