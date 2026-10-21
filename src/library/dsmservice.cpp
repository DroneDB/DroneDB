/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <cstdlib>
#include <mutex>
#include <ogrsf_frmts.h>
#include "dsmservice.h"
#include "exceptions.h"
#include "gdal_raii.h"
#include "utils.h"
#include <cpr/cpr.h>
#include "mio.h"
#include "constants.h"

#pragma clang diagnostic ignored "-Wdisabled-macro-expansion"

DSMService *DSMService::get()
{
    // Thread-safe initialization; destroyed at exit like the former atexit cleanup
    static DSMService instance;
    return &instance;
}

DSMService::DSMService()
{
}

DSMService::~DSMService()
{
}

std::optional<float> DSMService::lookupCache(double latitude, double longitude)
{
    const Point2D point(longitude, latitude);
    std::lock_guard<std::mutex> lock(cacheMutex);

    for (auto &it : cache)
    {
        if (it.second.bbox.contains(point))
        {
            // Skip entries whose raster data has not been loaded
            if (it.second.data.empty()) continue;

            float elevation = it.second.getElevation(latitude, longitude);
            if (!it.second.hasNodata || !utils::sameFloat(elevation, it.second.nodata))
            {
                return elevation;
            }
            else
            {
                LOGD << "DSM does not have a value for " << point;
                return 0.0f;
            }
        }
    }

    return std::nullopt;
}

bool DSMService::isCached(const std::string &filename)
{
    std::lock_guard<std::mutex> lock(cacheMutex);
    return cache.find(filename) != cache.end();
}

float DSMService::getAltitude(double latitude, double longitude)
{
    // Search cache
    if (const auto elevation = lookupCache(latitude, longitude))
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
    fs::path dsmCacheDir = getCacheDir();
    for (const auto &entry : fs::directory_iterator(dsmCacheDir))
    {

        // Not found in cache?
        std::string filename = ddb::io::toUtf8(entry.path().filename());
        if (!isCached(filename))
        {
            LOGD << "Adding " << ddb::io::toUtf8(entry.path()) << " to DSM service cache";
            try
            {
                if (addGeoTIFFToCache(entry.path(), latitude, longitude))
                {
                    return true; // Stop early, we've found a match
                }
            }
            catch (const GDALException)
            {
                LOGD << "Deleting " << ddb::io::toUtf8(entry.path()) << " because we can't open it";
                fs::remove(entry.path());
                std::lock_guard<std::mutex> lock(cacheMutex);
                cache.erase(filename);
            }
        }
    }

    return false; // No match
}

std::optional<std::string> buildDsmServiceUrl(const std::string &urlTemplate,
                                              const std::string &apiKey,
                                              double west, double south,
                                              double east, double north)
{
    if (apiKey.empty())
        return std::nullopt;

    std::string url = urlTemplate;
    utils::stringReplace(url, "{west}", std::to_string(west));
    utils::stringReplace(url, "{south}", std::to_string(south));
    utils::stringReplace(url, "{east}", std::to_string(east));
    utils::stringReplace(url, "{north}", std::to_string(north));
    utils::stringReplace(url, "{key}", apiKey);
    return url;
}

std::string dsmServiceApiKey()
{
    const char *value = std::getenv(DSM_SERVICE_KEY_ENV);
    std::string key = value != nullptr ? std::string(value) : std::string();
    // A key pasted with a trailing newline or spaces would end up in the URL
    utils::trim(key);
    return key;
}

std::string DSMService::loadFromNetwork(double latitude, double longitude)
{
    // TODO: allow user to specify different service
    return loadFromNetwork(latitude, longitude, DEFAULT_DSM_SERVICE_URL);
}

std::string DSMService::loadFromNetwork(double latitude, double longitude, const std::string &format)
{
    // Estimate bounds around point by a certain radius
    double radius = 5000.0; // meters

    UTMZone z = getUTMZone(latitude, longitude);
    Projected2D p = toUTM(latitude, longitude, z);

    Geographic2D max = fromUTM(Projected2D(p.x + radius, p.y + radius), z);
    Geographic2D min = fromUTM(Projected2D(p.x - radius, p.y - radius), z);

    const auto url = buildDsmServiceUrl(format, dsmServiceApiKey(),
                                        min.longitude, min.latitude, max.longitude, max.latitude);
    if (!url)
    {
        // Offline fallback: getAltitude() catches the NetException and returns 0. Warn once,
        // not for every image whose relative altitude is estimated.
        static std::once_flag warned;
        std::call_once(warned, [] {
            LOGW << "DSM download skipped: set " DSM_SERVICE_KEY_ENV
                    " to an OpenTopography API key to estimate relative altitudes from a world DSM";
        });
        throw NetException("No DSM service API key (" DSM_SERVICE_KEY_ENV " is not set)");
    }

    // Try to download
    std::string filename = std::to_string(static_cast<int>(p.x)) + "_" +
                           std::to_string(static_cast<int>(p.y)) + "_" +
                           std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".tif";
    std::string filePath = ddb::io::toUtf8(getCacheDir() / ddb::io::fromUtf8(filename));

    // The key stays out of the log, and out of the error message that getAltitude() logs
    const std::string redactedUrl =
        *buildDsmServiceUrl(format, "<redacted>", min.longitude, min.latitude, max.longitude, max.latitude);
    LOGD << "Downloading DSM from " << redactedUrl << " ...";

    try
    {
        utils::downloadToFile(*url, filePath, true);
    }
    catch (const NetException &e)
    {
        std::string message = e.what();
        utils::stringReplace(message, *url, redactedUrl);
        throw NetException(message);
    }

    // Verify the downloaded file exists and has content
    if (!fs::exists(ddb::io::fromUtf8(filePath)) || fs::file_size(ddb::io::fromUtf8(filePath)) == 0) {
        throw NetException("Downloaded DSM file is empty or does not exist: " + filePath);
    }

    return filePath;
}

bool DSMService::addGeoTIFFToCache(const fs::path &filePath, double latitude, double longitude)
{
    // Owned: the checks below throw while the dataset is open
    const auto dataset = adoptDataset(GDALOpen(ddb::io::toUtf8(filePath).c_str(), GA_ReadOnly));
    if (!dataset)
        throw GDALException("Cannot open " + ddb::io::toUtf8(filePath));
    GDALDatasetH hDataset = datasetHandle(dataset);

    DSMCacheEntry e;
    e.width = static_cast<unsigned int>(GDALGetRasterXSize(hDataset));
    e.height = static_cast<unsigned int>(GDALGetRasterYSize(hDataset));

    if (GDALGetGeoTransform(hDataset, e.geoTransform) != CE_None)
        throw GDALException("Cannot get geotransform for " + ddb::io::toUtf8(filePath));

    std::string wkt = GDALGetProjectionRef(hDataset);
    if (wkt.empty())
        throw GDALException("Cannot get projection ref for " + ddb::io::toUtf8(filePath));
    //    char *wktp = const_cast<char *>(wkt.c_str());
    // OGRSpatialReference *srs = new OGRSpatialReference();
    // if (srs->importFromWkt(&wktp) != OGRERR_NONE) throw GDALException("Cannot read spatial reference system for " + filePath.string() + ". Is PROJ installed?");

    // OGRSpatialReference *compare = new OGRSpatialReference();
    // compare->importFromEPSG(4326);

    // TODO: support for DSM with EPSG different than 4326
    // if (!srs->IsSame(compare)) throw GDALException("Cannot read DSM values from raster: " + filePath.string() + " (EPSG != 4326)");
    if (GDALGetRasterCount(hDataset) != 1)
        throw GDALException("More than 1 raster band found in elevation raster: " + ddb::io::toUtf8(filePath));

    GDALRasterBandH hBand = GDALGetRasterBand(hDataset, 1);
    e.nodata = static_cast<float>(GDALGetRasterNoDataValue(hBand, &e.hasNodata));

    Point2D min(0, e.height);
    Point2D max(e.width, 0);
    min.transform(e.geoTransform);
    max.transform(e.geoTransform);

    e.bbox.min = min;
    e.bbox.max = max;
    Point2D position(longitude, latitude);
    bool contained = e.bbox.contains(position);

    if (contained)
    {
        // Inside the boundaries, load data
        LOGD << position << " inside raster boundary, loading data from " << ddb::io::toUtf8(filePath);
        e.loadData(hDataset);
    }

    // Always cache the entry (at least with bbox) to avoid
    // repeated GDALOpen calls for files outside query bounds
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        cache[ddb::io::toUtf8(filePath.filename())] = std::move(e);
    }

    return contained;
}

fs::path DSMService::getCacheDir()
{
    return UserProfile::get()->getCachePath("dsm_service_cache");
}

void DSMCacheEntry::loadData(GDALDatasetH hDataset)
{
    GDALRasterBandH hBand = GDALGetRasterBand(hDataset, 1);
    data.clear();
    data.assign(width * height, 0.0f);
    if (GDALRasterIO(hBand, GF_Read, 0, 0, static_cast<int>(width),
                     static_cast<int>(height),
                     &data[0],
                     static_cast<int>(width),
                     static_cast<int>(height),
                     GDT_Float32, 0, 0) != CE_None)
    {
        throw GDALException("Cannot read raster data");
    }
}

float DSMCacheEntry::getElevation(double latitude, double longitude)
{
    // TODO: This could be empty if the DSM is not loaded, we should understand why this could happen
    if (data.empty())
        throw AppException("Cannot get elevation, need to call loadData() first.");
    if (width == 0 || height == 0)
        throw AppException("Cannot get elevation, need to populate width/height first.");

    double originX, originY, pixelSizeX, pixelSizeY;
    originX = geoTransform[0];
    originY = geoTransform[3];
    pixelSizeX = geoTransform[1];
    pixelSizeY = geoTransform[5];

    unsigned int pixelX = static_cast<unsigned int>((longitude - originX) / pixelSizeX);
    unsigned int pixelY = static_cast<unsigned int>((latitude - originY) / pixelSizeY);

    if (pixelX > width || pixelY > height)
    {
        throw AppException("Pixel coordinates (" +
                           std::to_string(pixelX) + "," + std::to_string(pixelY) + ") " +
                           "are outside of raster boundaries (" + std::to_string(width) + "x" + std::to_string(height) + ")");
    }

    return data[pixelX + pixelY * width];
}
