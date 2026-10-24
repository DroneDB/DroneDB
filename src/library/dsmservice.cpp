/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "dsmservice.h"

#include "constants.h"
#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "userprofile.h"

using namespace ddb;

DSMService *DSMService::get()
{
    // Thread-safe initialization; destroyed at exit like the former atexit cleanup
    static DSMService instance(defaultHttpClient(),
                               [] { return UserProfile::get()->getCachePath("dsm_service_cache"); });
    return &instance;
}

DSMService::DSMService(IHttpClient &client, std::function<fs::path()> cacheDirectory)
    : downloader(client), cacheDirectory(std::move(cacheDirectory))
{
}

float DSMService::getAltitude(double latitude, double longitude)
{
    // Search cache
    if (const auto elevation = memory.lookup(latitude, longitude))
        return *elevation;

    // TODO: we can probably optimize this
    // to lock based on the bounding box of the point
    {
        io::FileLock lock(getCacheDir() / ".." / "dsm_service");

        // Load existing DSMs in cache until we find a matching one
        if (loadDiskCache(latitude, longitude))
        {
            return getAltitude(latitude, longitude);
        }

        // Attempt to load from the network and recurse
        try
        {
            if (addGeoTIFFToCache(ddb::io::fromUtf8(loadFromNetwork(latitude, longitude)), latitude, longitude))
            {
                return getAltitude(latitude, longitude);
            }
        }
        catch (const NetException &e)
        {
            LOGD << e.what();
        }
    }

    LOGD << "Cannot get elevation from DSM service";
    return 0;
}

bool DSMService::loadDiskCache(double latitude, double longitude)
{
    return loadDsmFolder(getCacheDir(), memory, latitude, longitude);
}

std::string DSMService::loadFromNetwork(double latitude, double longitude)
{
    // TODO: allow user to specify different service
    return loadFromNetwork(latitude, longitude, DEFAULT_DSM_SERVICE_URL);
}

std::string DSMService::loadFromNetwork(double latitude, double longitude, const std::string &urlTemplate)
{
    DsmDownload request;
    request.latitude = latitude;
    request.longitude = longitude;
    request.urlTemplate = urlTemplate;
    request.apiKey = dsmServiceApiKey();
    request.folder = getCacheDir();
    return downloader.download(request);
}

bool DSMService::addGeoTIFFToCache(const fs::path &filePath, double latitude, double longitude)
{
    return addRasterToCache(memory, filePath, latitude, longitude);
}

fs::path DSMService::getCacheDir()
{
    return cacheDirectory();
}
