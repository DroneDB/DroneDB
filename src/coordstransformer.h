/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef COORDSTRANSFORMER_H
#define COORDSTRANSFORMER_H

#include <gdal_priv.h>
#include <ogr_srs_api.h>
#include "ddb_export.h"

namespace ddb{


class CoordsTransformer{
    OGRSpatialReferenceH hSrc = OSRNewSpatialReference(nullptr);
    OGRSpatialReferenceH hTgt = OSRNewSpatialReference(nullptr);
    OGRCoordinateTransformationH hTransform;
public:
    DDB_DLL CoordsTransformer(int epsgFrom, int epsgTo);
    DDB_DLL CoordsTransformer(const std::string &wktFrom, int epsgTo);
    DDB_DLL CoordsTransformer(int epsgFrom, const std::string &wktTo);
    DDB_DLL ~CoordsTransformer();

    DDB_DLL void transform(double *x, double *y);
    DDB_DLL void transform(double *x, double *y, double *z);
};

}

#endif // COORDSTRANSFORMER_H
