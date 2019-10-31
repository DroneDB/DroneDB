#include "dsmservice.h"
#include "userprofile.h"
#include "exceptions.h"

DSMService::DSMService(){

}

float DSMService::getAltitude(double latitude, double longitude){
    geo::Point2D point(longitude, latitude);

    // Search cache
    for (auto &it : cache){
        if (it.second.bbox.contains(point)){
            return it.second.getElevation(latitude, longitude);
        }
    }

    // Load existing DSMs in cache until we find a matching one
    if (loadDiskCache(latitude, longitude)){
        return getAltitude(latitude, longitude);
    }

    // TODO: load from network

    return 0;
}

bool DSMService::loadDiskCache(double latitude, double longitude){
    fs::path dsmCacheDir = UserProfile::Instance()->getProfilePath("dsm_service_cache", true);
    for (const auto &entry : fs::directory_iterator(dsmCacheDir)){

        // Not found in cache?
        std::string filename = entry.path().filename().string();
        if (cache.find(filename) == cache.end()){
            LOGD << "Adding " << entry.path().string() << " to DSM service cache";
            if (addGeoTIFFToCache(filename, latitude, longitude)){
                return true; // Stop early, we've found a match
            }
        }
    }

    return false; // No match
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
    char *wktp = const_cast<char *>(wkt.c_str());
    OGRSpatialReference *srs = new OGRSpatialReference();
    if (srs->importFromWkt(&wktp) != OGRERR_NONE) throw GDALException("Cannot read spatial reference system for " + filePath.string() + ". Is PROJ installed?");

    OGRSpatialReference *compare = new OGRSpatialReference();
    compare->importFromEPSG(4326);

    // TODO: support for DSM with EPSG different than 4326
    if (!srs->IsSame(compare)) throw GDALException("Cannot read DSM values from raster: " + filePath.string() + " (EPSG != 4326)");
    if (dataset->GetRasterCount() != 1) throw GDALException("More than 1 raster band found in elevation raster: " + filePath.string());

    geo::Point2D min(0, e.height);
    geo::Point2D max(e.width, 0);
    min.transform(e.geoTransform);
    max.transform(e.geoTransform);

    e.bbox.min = min;
    e.bbox.max = max;
    geo::Point2D position(longitude, latitude);
    bool contained = e.bbox.contains(position);

    if (contained){
        // Inside the boundaries, load data
        LOGD << position << " inside raster boundary, loading data from " << filePath.string();
        e.loadData(dataset);
    }

    cache[filePath.filename().string()] = e;
    GDALClose(dataset);

    return contained;
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
