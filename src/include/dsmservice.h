/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef DSMSERVICE_H
#define DSMSERVICE_H

#include <string>
#include <vector>
#include <cstring>
#include <unordered_map>
#include <filesystem>
#include <mutex>
#include <optional>
#include "gdal_inc.h"
#include "userprofile.h"
#include "geo.h"
#include "ddb_export.h"

struct DSMCacheEntry
{
    unsigned int width, height;
    int hasNodata;
    float nodata;

    double geoTransform[6];
    std::vector<float> data;
    ddb::BoundingBox<ddb::Point2D> bbox;

    DDB_DLL DSMCacheEntry() : width(0), height(0), hasNodata(0), nodata(0)
    {
        memset(geoTransform, 0, sizeof(double) * 6);
    }

    DDB_DLL void loadData(GDALDatasetH hDataset);
    DDB_DLL float getElevation(double latitude, double longitude);
};

// Fills the {west}, {south}, {east}, {north} and {key} placeholders of a DSM service URL
// template. Returns nothing when apiKey is empty: the service refuses keyless requests.
DDB_DLL std::optional<std::string> buildDsmServiceUrl(const std::string &urlTemplate,
                                                      const std::string &apiKey,
                                                      double west, double south,
                                                      double east, double north);

// The DSM service API key from DSM_SERVICE_KEY_ENV, read at call time and trimmed ("" when unset or blank)
DDB_DLL std::string dsmServiceApiKey();

class DSMService
{
    // filename --> cache entry. cacheMutex guards every access and is held only around
    // the map itself: getAltitude() recurses through loadDiskCache()/addGeoTIFFToCache(),
    // so no lock may be held across those calls.
    std::unordered_map<std::string, DSMCacheEntry> cache;
    std::mutex cacheMutex;

    DSMService();
    ~DSMService();

    // The elevation from a loaded cache entry containing the point (0 when the DSM has no
    // value there), or nothing when no loaded entry contains it.
    std::optional<float> lookupCache(double latitude, double longitude);
    bool isCached(const std::string &filename);

public:
    DDB_DLL static DSMService *get();

    DDB_DLL float getAltitude(double latitude, double longitude);

    DDB_DLL bool loadDiskCache(double latitude, double longitude);
    DDB_DLL std::string loadFromNetwork(double latitude, double longitude);
    // Same, from the given service URL template (see buildDsmServiceUrl)
    DDB_DLL std::string loadFromNetwork(double latitude, double longitude, const std::string &urlTemplate);
    DDB_DLL bool addGeoTIFFToCache(const fs::path &filePath, double latitude, double longitude);
    DDB_DLL fs::path getCacheDir();
};

#endif // DSMSERVICE_H
