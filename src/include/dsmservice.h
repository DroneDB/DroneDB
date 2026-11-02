/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef DSMSERVICE_H
#define DSMSERVICE_H

// Elevations from a world DSM: the DSM rasters held in memory (dsm_cache.h), those downloaded
// before into a folder of the disk, and a download of the DSM around the point when neither has
// it (dsm_downloader.h, over an injected IHttpClient).

#include <functional>
#include <string>

#include "dsm_cache.h"
#include "dsm_downloader.h"
#include "dsm_reader.h"
#include "fs.h"
#include "http_client.h"

class DSMService
{
    ddb::DsmMemoryCache memory;
    ddb::DsmDownloader downloader;
    std::function<fs::path()> cacheDirectory;

public:
    // The service of the process: the user's cache folder and the library's HTTP client
    static DSMService *get();

    // A service with its own HTTP client (it must outlive the service) and cache folder
    DSMService(ddb::IHttpClient &client, std::function<fs::path()> cacheFolder);

    float getAltitude(double latitude, double longitude);

    bool loadDiskCache(double latitude, double longitude);
    std::string loadFromNetwork(double latitude, double longitude);
    // Same, from the given service URL template (see buildDsmServiceUrl)
    std::string loadFromNetwork(double latitude, double longitude, const std::string &urlTemplate);
    bool addGeoTIFFToCache(const fs::path &filePath, double latitude, double longitude);
    fs::path getCacheDir();
};

#endif // DSMSERVICE_H
