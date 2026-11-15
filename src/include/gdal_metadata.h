/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef GDAL_METADATA_H
#define GDAL_METADATA_H

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gdal.h>

// The metadata strings of a raster that keyword detection (thermal rasters, sensor profiles)
// scans. Only what describes the data is returned: GDAL also lists the domains of the derived
// and sub datasets, which hold the path of the file, and a path says nothing about the sensor
// (a folder called "subtitles" is not a thermal raster).

namespace ddb {
class RasterSource;
}

namespace ddb::gdalmeta {

// A metadata domain of a raster: its name and its items (GDALGetMetadata: "KEY=VALUE" items, or the
// document of an xml: or json: domain)
struct Domain {
    std::string name;
    std::vector<std::string> items;
    bool operator==(const Domain &) const = default;
};

// The metadata of a raster as GDAL lists it: the domains of the dataset (GDALGetMetadataDomainList, in
// its order, each with GDALGetMetadata) and the items of the default domain of each band
struct RasterMetadata {
    std::vector<Domain> domains;
    std::vector<std::vector<std::string>> bandItems;
    bool operator==(const RasterMetadata &) const = default;
};

// The metadata of a dataset GDAL opened
RasterMetadata metadataOf(GDALDatasetH dataset);

// The value of item `name` of `domain` as GDALGetMetadataItem finds it (the name in any case, then
// '=' or ':'), or none
std::optional<std::string> itemOf(const RasterMetadata &metadata, std::string_view domain, std::string_view name);

struct CollectOptions {
    // Metadata of bands 1..bandCount
    int bandCount = 0;
    // The description of each of those bands, as well as its metadata
    bool bandDescriptions = false;
    // Items of the default domain of the dataset that are returned first, when they exist
    std::vector<std::string> datasetItems;
};

// The metadata strings of a raster: the requested items, every domain of the dataset except the
// ones that hold file paths, and the requested bands. An entry is "KEY=VALUE" or a plain string;
// an entry whose value is a path or a URL is left out.
std::vector<std::string> collectMetadata(const RasterSource &raster, const CollectOptions &options);

// Whether a metadata value is a file path or a URL: it starts with '/' (which includes /vsi
// paths) or '\' (a UNC path), with a drive letter and ':', or with "<scheme>://" (a value
// that only contains a URL, such as an XMP packet or a description, is text)
bool isPathOrUrl(std::string_view value);

// Whether `token` is a whole word of `text`, where a word is a maximal run of ASCII letters and
// digits: "bt" is in "BT_band 10" and "bt-2" but not in "subtitles". Case-insensitive.
bool containsToken(std::string_view text, std::string_view token);

} // namespace ddb::gdalmeta

#endif
