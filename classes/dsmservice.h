/* Copyright 2019 MasseranoLabs LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#ifndef DSMSERVICE_H
#define DSMSERVICE_H

#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <gdal_priv.h>
#include <ogr_spatialref.h>
#include <cpl_conv.h>
#include "userprofile.h"
#include "../libs/geo.h"

using namespace ddb;

struct DSMCacheEntry{
    geo::BoundingBox<geo::Point2D> bbox;
    unsigned int width, height;
    double geoTransform[6];
    std::vector<float> data;

    int hasNodata;
    float nodata;

    DSMCacheEntry() : width(0), height(0) {}

    void loadData(GDALDataset *dataset);
    float getElevation(double latitude, double longitude);
};

class DSMService{
    std::unordered_map<std::string, DSMCacheEntry> cache; // filename --> cache entry
    DSMService();
    ~DSMService();
    static DSMService *instance;
public:
    static DSMService* get();

    float getAltitude(double latitude, double longitude);

    bool loadDiskCache(double latitude, double longitude);
    std::string loadFromNetwork(double latitude, double longitude);
    bool addGeoTIFFToCache(const fs::path &filePath, double latitude, double longitude);
    void downloadFile(const std::string &url, const std::string &outFile);
    fs::path getCacheDir();
};

#endif // DSMSERVICE_H
