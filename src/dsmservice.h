/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

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
#include "geo.h"
#include "ddb_export.h"

using namespace ddb;

struct DSMCacheEntry{
    unsigned int width, height;
    int hasNodata;
    float nodata;

	double geoTransform[6];
	std::vector<float> data;
	BoundingBox<Point2D> bbox;

    DDB_DLL DSMCacheEntry() : width(0), height(0), hasNodata(0), nodata(0) {
		memset(geoTransform, 0, sizeof(double)*6);
	}

    DDB_DLL void loadData(GDALDataset *dataset);
    DDB_DLL float getElevation(double latitude, double longitude);
};

class DSMService{
    std::unordered_map<std::string, DSMCacheEntry> cache; // filename --> cache entry
    DSMService();
    ~DSMService();
    static DSMService *instance;
public:
    DDB_DLL static DSMService* get();

    DDB_DLL float getAltitude(double latitude, double longitude);

    DDB_DLL bool loadDiskCache(double latitude, double longitude);
    DDB_DLL std::string loadFromNetwork(double latitude, double longitude);
    DDB_DLL bool addGeoTIFFToCache(const fs::path &filePath, double latitude, double longitude);
    DDB_DLL fs::path getCacheDir();
};

#endif // DSMSERVICE_H
