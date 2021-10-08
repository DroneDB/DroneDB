/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef GDALTILER_H
#define GDALTILER_H

#include "gdal_inc.h"

#include <sstream>
#include <string>

#include "ddb_export.h"
#include "tiler.h"

namespace ddb {

struct GeoExtent {
    int x;
    int y;
    int xsize;
    int ysize;
};

struct GQResult {
    GeoExtent r;
    GeoExtent w;
};

class GDALTiler : public Tiler {
    GDALDriverH pngDrv;
    GDALDriverH memDrv;

    GDALDatasetH inputDataset = nullptr;
    GDALDatasetH origDataset = nullptr;
    
    int rasterCount;

    bool hasGeoreference(const GDALDatasetH &dataset);
    bool sameProjection(const OGRSpatialReferenceH &a,
                        const OGRSpatialReferenceH &b);
    int dataBandsCount(const GDALDatasetH &dataset);
    GDALDatasetH createWarpedVRT(
        const GDALDatasetH &src, const OGRSpatialReferenceH &srs,
        GDALResampleAlg resampling = GRA_NearestNeighbour);

    // Returns parameters reading raster data.
    // (coordinates and x/y shifts for border tiles).
    // If the querysize is not given, the
    // extent is returned in the native resolution of dataset ds.
    GQResult geoQuery(GDALDatasetH ds, double ulx, double uly, double lrx,
                      double lry, int querySize = 0);

    template <typename T>
    void rescale(GDALRasterBandH hBand, char *buffer, size_t bufsize);
   public:
    DDB_DLL GDALTiler(const std::string &geotiffPath,
                  const std::string &outputFolder, int tileSize = 256,
                  bool tms = false);
    DDB_DLL ~GDALTiler();

    DDB_DLL std::string tile(int tz, int tx, int ty, uint8_t **outBuffer = nullptr, int *outBufferSize = nullptr) override;
};

}  // namespace ddb

#endif  // GDALTILER_H
