/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TILER_H
#define TILER_H

#include <gdal_priv.h>
#include <ogr_srs_api.h>
#include <string>

namespace ddb{

class Tiler
{
    std::string geotiffPath;

    bool hasGeoreference(const GDALDatasetH &dataset);
    bool sameProjection(const OGRSpatialReferenceH &a, const OGRSpatialReferenceH &b);
public:
    Tiler(const std::string &geotiffPath);

    std::string tile(int x, int y, int z);
};

}

#endif // TILER_H
