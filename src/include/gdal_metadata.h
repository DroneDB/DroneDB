/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef GDAL_METADATA_H
#define GDAL_METADATA_H

#include <string>
#include <string_view>
#include <vector>

#include <gdal.h>

// The metadata strings of a raster that keyword detection (thermal rasters, sensor profiles)
// scans. Only what describes the data is returned: GDAL also lists the domains of the derived
// and sub datasets, which hold the path of the file, and a path says nothing about the sensor
// (a folder called "subtitles" is not a thermal raster).

namespace ddb::gdalmeta {

struct CollectOptions {
    // Metadata of bands 1..bandCount
    int bandCount = 0;
    // The description of each of those bands, as well as its metadata
    bool bandDescriptions = false;
    // Items of the default domain of the dataset that are returned first, when they exist
    std::vector<std::string> datasetItems;
};

// The metadata strings of the dataset: the requested items, every domain of the dataset except
// the ones that hold file paths, and the requested bands. An entry is "KEY=VALUE" or a plain
// string; an entry whose value is a path or a URL is left out.
std::vector<std::string> collectMetadata(GDALDatasetH dataset, const CollectOptions &options);

// Whether a metadata value is a file path or a URL: it starts with '/' (which includes /vsi
// paths) or '\' (a UNC path), with a drive letter and ':', or contains "://"
bool isPathOrUrl(std::string_view value);

// Whether `token` is a whole word of `text`, where a word is a maximal run of ASCII letters and
// digits: "bt" is in "BT_band 10" and "bt-2" but not in "subtitles". Case-insensitive.
bool containsToken(std::string_view text, std::string_view token);

} // namespace ddb::gdalmeta

#endif
