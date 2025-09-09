/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <sstream>
#include <memory>
#include "coordstransformer.h"
#include "exceptions.h"

namespace ddb
{

    CoordsTransformer::CoordsTransformer(int epsgFrom, int epsgTo)
    {
        if (OSRImportFromEPSG(hSrc, epsgFrom) != OGRERR_NONE)
            throw GDALException("Cannot import spatial reference system " + std::to_string(epsgFrom) + ". Is PROJ available?");

        if (OSRImportFromEPSG(hTgt, epsgTo) != OGRERR_NONE)
            throw GDALException("Cannot import spatial reference system " + std::to_string(epsgTo) + ". Is PROJ available?");

        hTransform = OCTNewCoordinateTransformation(hSrc, hTgt);
        if (!hTransform)
            throw GDALException("Failed to create coordinate transformation");
    }

    CoordsTransformer::CoordsTransformer(const std::string &wktFrom, int epsgTo)
    {

        std::unique_ptr<char, void(*)(void*)> wktOwner(strdup(wktFrom.c_str()), std::free);

        char* wktPtr = wktOwner.get();
        if (OSRImportFromWkt(hSrc, &wktPtr) != OGRERR_NONE)
            throw GDALException("Cannot import spatial reference system " + wktFrom + ". Is PROJ available?");

        if (OSRImportFromEPSG(hTgt, epsgTo) != OGRERR_NONE)
            throw GDALException("Cannot import spatial reference system " + std::to_string(epsgTo) + ". Is PROJ available?");

        hTransform = OCTNewCoordinateTransformation(hSrc, hTgt);
        if (!hTransform)
            throw GDALException("Failed to create coordinate transformation");
    }

    CoordsTransformer::CoordsTransformer(int epsgFrom, const std::string &wktTo)
    {
        if (OSRImportFromEPSG(hSrc, epsgFrom) != OGRERR_NONE)
            throw GDALException("Cannot import spatial reference system " + std::to_string(epsgFrom) + ". Is PROJ available?");

        std::unique_ptr<char, void(*)(void*)> wktOwner(strdup(wktTo.c_str()), std::free);

        char* wktPtr = wktOwner.get();
        if (OSRImportFromWkt(hTgt, &wktPtr) != OGRERR_NONE)
            throw GDALException("Cannot import spatial reference system " + wktTo + ". Is PROJ available?");

        hTransform = OCTNewCoordinateTransformation(hSrc, hTgt);
        if (!hTransform)
            throw GDALException("Failed to create coordinate transformation");
    }

    CoordsTransformer::~CoordsTransformer()
    {
        if (hTransform)
            OCTDestroyCoordinateTransformation(hTransform);
        if (hTgt)
            OSRDestroySpatialReference(hTgt);
        if (hSrc)
            OSRDestroySpatialReference(hSrc);
    }

    void CoordsTransformer::transform(double *x, double *y)
    {
        double none = 0.0;
        if (!OCTTransform(hTransform, 1, x, y, &none))
            throw GDALException("Transform failed");

    }

    void CoordsTransformer::transform(double *x, double *y, double *z)
    {
        if (!OCTTransform(hTransform, 1, x, y, z))
            throw GDALException("Transform failed");

    }

}
