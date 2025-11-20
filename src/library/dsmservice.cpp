/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <regex>
#include <cstdlib>
#include <ogrsf_frmts.h>
#include "dsmservice.h"
#include "exceptions.h"
#include "utils.h"
#include <cpr/cpr.h>
#include "mio.h"
#include "constants.h"

#pragma clang diagnostic ignored "-Wdisabled-macro-expansion"

DSMService *DSMService::instance = nullptr;

DSMService *DSMService::get()
{
    if (!instance)
    {
        instance = new DSMService();
        // Register cleanup at program exit
        std::atexit(cleanup);
    }

    return instance;
}

void DSMService::cleanup()
{
    delete instance;
    instance = nullptr;
}

DSMService::DSMService()
{
}

DSMService::~DSMService()
{
}

float DSMService::getAltitude(double latitude, double longitude)
{
    Point2D point(longitude, latitude);

    // Search cache
    for (auto &it : cache)
    {
        if (it.second.bbox.contains(point))
        {
            float elevation = it.second.getElevation(latitude, longitude);
            if (!it.second.hasNodata || !utils::sameFloat(elevation, it.second.nodata))
            {
                return elevation;
            }
            else
            {
                LOGD << "DSM does not have a value for " << point;
                return 0.0;
            }
        }
    }

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
            if (addGeoTIFFToCache(loadFromNetwork(latitude, longitude), latitude, longitude))
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
        std::string filename = entry.path().filename().string();
        if (cache.find(filename) == cache.end())
        {
            LOGD << "Adding " << entry.path().string() << " to DSM service cache";
            try
            {
                if (addGeoTIFFToCache(entry.path(), latitude, longitude))
                {
                    return true; // Stop early, we've found a match
                }
            }
            catch (const GDALException)
            {
                LOGD << "Deleting " << entry.path().string() << " because we can't open it";
                fs::remove(entry.path());
                cache.erase(filename);
            }
        }
    }

    return false; // No match
}

std::string DSMService::loadFromNetwork(double latitude, double longitude)
{
    // TODO: allow user to specify different service
    std::string format = DEFAULT_DSM_SERVICE_URL;

    // Estimate bounds around point by a certain radius
    double radius = 5000.0; // meters

    UTMZone z = getUTMZone(latitude, longitude);
    Projected2D p = toUTM(latitude, longitude, z);

    Geographic2D max = fromUTM(Projected2D(p.x + radius, p.y + radius), z);
    Geographic2D min = fromUTM(Projected2D(p.x - radius, p.y - radius), z);

    std::string url = std::regex_replace(format, std::regex("\\{west\\}"), std::to_string(min.longitude));
    url = std::regex_replace(url, std::regex("\\{east\\}"), std::to_string(max.longitude));
    url = std::regex_replace(url, std::regex("\\{north\\}"), std::to_string(max.latitude));
    url = std::regex_replace(url, std::regex("\\{south\\}"), std::to_string(min.latitude));

    // Try to download
    std::string filename = std::to_string(static_cast<int>(p.x)) + "_" +
                           std::to_string(static_cast<int>(p.y)) + "_" +
                           std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".tif";
    std::string filePath = (getCacheDir() / filename).string();

    LOGD << "Downloading DSM from " << url << " ...";

    auto response = utils::downloadToFile(url, filePath, true);

    // Verify the downloaded file exists and has content
    if (!fs::exists(filePath) || fs::file_size(filePath) == 0) {
        throw NetException("Downloaded DSM file is empty or does not exist: " + filePath);
    }

    return filePath;
}

bool DSMService::addGeoTIFFToCache(const fs::path &filePath, double latitude, double longitude)
{
    GDALDatasetH hDataset = GDALOpen(filePath.string().c_str(), GA_ReadOnly);
    if (hDataset == nullptr)
        throw GDALException("Cannot open " + filePath.string());

    DSMCacheEntry e;
    e.width = static_cast<unsigned int>(GDALGetRasterXSize(hDataset));
    e.height = static_cast<unsigned int>(GDALGetRasterYSize(hDataset));

    if (GDALGetGeoTransform(hDataset, e.geoTransform) != CE_None)
        throw GDALException("Cannot get geotransform for " + filePath.string());

    std::string wkt = GDALGetProjectionRef(hDataset);
    if (wkt.empty())
        throw GDALException("Cannot get projection ref for " + filePath.string());
    //    char *wktp = const_cast<char *>(wkt.c_str());
    // OGRSpatialReference *srs = new OGRSpatialReference();
    // if (srs->importFromWkt(&wktp) != OGRERR_NONE) throw GDALException("Cannot read spatial reference system for " + filePath.string() + ". Is PROJ installed?");

    // OGRSpatialReference *compare = new OGRSpatialReference();
    // compare->importFromEPSG(4326);

    // TODO: support for DSM with EPSG different than 4326
    // if (!srs->IsSame(compare)) throw GDALException("Cannot read DSM values from raster: " + filePath.string() + " (EPSG != 4326)");
    if (GDALGetRasterCount(hDataset) != 1)
        throw GDALException("More than 1 raster band found in elevation raster: " + filePath.string());

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
        LOGD << position << " inside raster boundary, loading data from " << filePath.string();
        e.loadData(hDataset);
    }

    cache[filePath.filename().string()] = e;
    GDALClose(hDataset);
    // delete srs;
    // delete compare;

    return contained;
}

fs::path DSMService::getCacheDir()
{
    return UserProfile::get()->getProfilePath("dsm_service_cache", true);
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
