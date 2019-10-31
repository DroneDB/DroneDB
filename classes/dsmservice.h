#ifndef DSMSERVICE_H
#define DSMSERVICE_H

#include <string>
#include <vector>
#include <unordered_map>
#include <gdal_priv.h>
#include <ogr_spatialref.h>
#include <cpl_conv.h>
#include "../libs/geo.h"


struct DSMCacheEntry{
    geo::BoundingBox<geo::Point2D> bbox;
    unsigned int width, height;
    double geoTransform[6];
    std::vector<float> data;

    DSMCacheEntry() : width(0), height(0) {}

    void loadData(GDALDataset *dataset);
    float getElevation(double latitude, double longitude);
};

class DSMService{
    std::unordered_map<std::string, DSMCacheEntry> cache; // filename --> cache entry
public:
    DSMService();

    float getAltitude(double latitude, double longitude);

    bool loadDiskCache(double latitude, double longitude);
    bool addGeoTIFFToCache(const fs::path &filePath, double latitude, double longitude);

};

#endif // DSMSERVICE_H
