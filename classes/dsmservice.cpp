/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <regex>
#include <ogrsf_frmts.h>
#include <curl/curl.h>
#include "dsmservice.h"
#include "exceptions.h"
#include "../utils.h"

#pragma clang diagnostic ignored "-Wdisabled-macro-expansion"

DSMService *DSMService::instance = nullptr;

DSMService *DSMService::get(){
    if (!instance){
        instance = new DSMService();
    }

    return instance;
}

DSMService::DSMService(){
    curl_global_init(CURL_GLOBAL_ALL);
}

DSMService::~DSMService(){
    curl_global_cleanup();
}

float DSMService::getAltitude(double latitude, double longitude){
    Point2D point(longitude, latitude);

    // Search cache
    for (auto &it : cache){
        if (it.second.bbox.contains(point)){
            float elevation = it.second.getElevation(latitude, longitude);
            if (!it.second.hasNodata || !utils::sameFloat(elevation, it.second.nodata)){
                return elevation;
            }else{
                LOGW << "DSM does not have a value for " << point;
                return 0.0;
            }
        }
    }

    // Load existing DSMs in cache until we find a matching one
    if (loadDiskCache(latitude, longitude)){
        return getAltitude(latitude, longitude);
    }

    // Attempt to load from the network and recurse
    if (addGeoTIFFToCache(loadFromNetwork(latitude, longitude), latitude, longitude)){
        return getAltitude(latitude, longitude);
    }

    LOGW << "Cannot get elevation from DSM service";
    return 0;
}

bool DSMService::loadDiskCache(double latitude, double longitude){
    fs::path dsmCacheDir = getCacheDir();
    for (const auto &entry : fs::directory_iterator(dsmCacheDir)){

        // Not found in cache?
        std::string filename = entry.path().filename().string();
        if (cache.find(filename) == cache.end()){
            LOGD << "Adding " << entry.path().string() << " to DSM service cache";
            try {
                if (addGeoTIFFToCache(entry.path(), latitude, longitude)){
                    return true; // Stop early, we've found a match
                }
            } catch (const GDALException) {
                LOGD << "Deleting " << entry.path().string() << " because we can't open it";
                fs::remove(entry.path());
                cache.erase(filename);
            }
        }
    }

    return false; // No match
}

std::string DSMService::loadFromNetwork(double latitude, double longitude){
    // TODO: allow user to specify different service
    std::string format = "http://opentopo.sdsc.edu/otr/getdem?demtype=AW3D30&west={west}&south={south}&east={east}&north={north}&outputFormat=GTiff";

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

    std::cout << "Downloading DSM from " << url << " ..." << std::endl;
    downloadFile(url, filePath);

    return filePath;
}

bool DSMService::addGeoTIFFToCache(const fs::path &filePath, double latitude, double longitude){
    GDALDataset *dataset = static_cast<GDALDataset *>(GDALOpen(filePath.string().c_str(), GA_ReadOnly));
    if( dataset == nullptr ) throw GDALException("Cannot open " + filePath.string());

    DSMCacheEntry e;
    e.width = static_cast<unsigned int>(dataset->GetRasterXSize());
    e.height = static_cast<unsigned int>(dataset->GetRasterYSize());

    if (dataset->GetGeoTransform(e.geoTransform) != CE_None) throw GDALException("Cannot get geotransform for " + filePath.string());

    std::string wkt = GDALGetProjectionRef(dataset);
    if (wkt.empty()) throw GDALException("Cannot get projection ref for " + filePath.string());
    const char *wktp = const_cast<char *>(wkt.c_str());
    OGRSpatialReference *srs = new OGRSpatialReference();
    if (srs->importFromWkt(&wktp) != OGRERR_NONE) throw GDALException("Cannot read spatial reference system for " + filePath.string() + ". Is PROJ installed?");

    OGRSpatialReference *compare = new OGRSpatialReference();
    compare->importFromEPSG(4326);

    // TODO: support for DSM with EPSG different than 4326
    if (!srs->IsSame(compare)) throw GDALException("Cannot read DSM values from raster: " + filePath.string() + " (EPSG != 4326)");
    if (dataset->GetRasterCount() != 1) throw GDALException("More than 1 raster band found in elevation raster: " + filePath.string());

    e.nodata = static_cast<float>(dataset->GetRasterBand(1)->GetNoDataValue(&e.hasNodata));

    Point2D min(0, e.height);
    Point2D max(e.width, 0);
    min.transform(e.geoTransform);
    max.transform(e.geoTransform);

    e.bbox.min = min;
    e.bbox.max = max;
    Point2D position(longitude, latitude);
    bool contained = e.bbox.contains(position);

    if (contained){
        // Inside the boundaries, load data
        LOGD << position << " inside raster boundary, loading data from " << filePath.string();
        e.loadData(dataset);
    }

    cache[filePath.filename().string()] = e;
    GDALClose(dataset);
    delete srs;
    delete compare;

    return contained;
}

void DSMService::downloadFile(const std::string &url, const std::string &outFile){
    CURL *curl = nullptr;
    FILE *f = nullptr;

    try {
        curl = curl_easy_init();
        if (!curl) throw CURLException("Cannot initialize CURL");

        f = fopen(outFile.c_str(), "wb");
        if (!f) throw FSException("Cannot open " + outFile + " for writing");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

        if (is_logger_verbose()){
            curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
        }

        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, nullptr);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        if (curl_easy_perform(curl) != CURLE_OK) throw CURLException("Cannot download " + url + ", perhaps the service is offline or unreachable.");

        curl_easy_cleanup(curl);
        curl = nullptr;

        fclose(f);
        f = nullptr;
    }catch (AppException &e) {
        if (curl) curl_easy_cleanup(curl);
        if (f) fclose(f);
        throw e;
    }
}

fs::path DSMService::getCacheDir(){
    return UserProfile::get()->getProfilePath("dsm_service_cache", true);
}

void DSMCacheEntry::loadData(GDALDataset *dataset){
    GDALRasterBand *band = dataset->GetRasterBand(1);
    data.clear();
    data.assign(width * height, 0.0f);
    if (band->RasterIO(GF_Read, 0, 0, static_cast<int>(width),
                                  static_cast<int>(height),
                                  &data[0],
                                  static_cast<int>(width),
                                  static_cast<int>(height),
                       GDT_Float32, 0, 0) != CE_None){
        throw GDALException("Cannot read raster data");
    }
}

float DSMCacheEntry::getElevation(double latitude, double longitude){
    if (data.empty()) throw AppException("Cannot get elevation, need to call loadData() first.");
    if (width == 0 || height == 0) throw AppException("Cannot get elevation, need to populate width/height first.");

    double originX, originY, pixelSizeX, pixelSizeY;
    originX = geoTransform[0];
    originY = geoTransform[3];
    pixelSizeX = geoTransform[1];
    pixelSizeY = geoTransform[5];

    unsigned int pixelX = static_cast<unsigned int>((longitude - originX) / pixelSizeX);
    unsigned int pixelY = static_cast<unsigned int>((latitude - originY) / pixelSizeY);

    if (pixelX > width || pixelY > height) {
      throw AppException("Pixel coordinates (" +
                         std::to_string(pixelX) + "," + std::to_string(pixelY) + ") " +
                         "are outside of raster boundaries (" + std::to_string(width) + "x" + std::to_string(height) + ")");
    }

    return data[pixelX + pixelY * width];
}
