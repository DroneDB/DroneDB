/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <cmath>
#include <vector>
#include "coordstransformer.h"
#include "exceptions.h"

namespace ddb
{
    namespace
    {
        void importEpsg(OGRSpatialReference &srs, int epsg)
        {
            if (srs.importFromEPSG(epsg) != OGRERR_NONE)
                throw GDALException("Cannot import spatial reference system " + std::to_string(epsg) + ". Is PROJ available?");
        }

        void importWkt(OGRSpatialReference &srs, const std::string &wkt)
        {
            if (srs.importFromWkt(wkt.c_str()) != OGRERR_NONE)
                throw GDALException("Cannot import spatial reference system " + wkt + ". Is PROJ available?");
        }
    }

    void CoordsTransformer::createTransform()
    {
        ct.reset(OGRCreateCoordinateTransformation(&src, &tgt));
        if (!ct)
            throw GDALException("Failed to create coordinate transformation");
    }

    CoordsTransformer::CoordsTransformer(int epsgFrom, int epsgTo)
    {
        importEpsg(src, epsgFrom);
        importEpsg(tgt, epsgTo);
        createTransform();
    }

    CoordsTransformer::CoordsTransformer(const std::string &wktFrom, int epsgTo)
    {
        importWkt(src, wktFrom);
        importEpsg(tgt, epsgTo);
        createTransform();
    }

    CoordsTransformer::CoordsTransformer(int epsgFrom, const std::string &wktTo)
    {
        importEpsg(src, epsgFrom);
        importWkt(tgt, wktTo);
        createTransform();
    }

    CoordsTransformer::~CoordsTransformer() = default;
    CoordsTransformer::CoordsTransformer(CoordsTransformer &&) noexcept = default;
    CoordsTransformer &CoordsTransformer::operator=(CoordsTransformer &&) noexcept = default;

    void CoordsTransformer::transform(double *x, double *y)
    {
        double none = 0.0;
        if (!ct->Transform(1, x, y, &none))
            throw GDALException("Transform failed");
    }

    void CoordsTransformer::transform(double *x, double *y, double *z)
    {
        if (!ct->Transform(1, x, y, z))
            throw GDALException("Transform failed");
    }

    void CoordsTransformer::transform(size_t n, double *x, double *y)
    {
        if (n == 0)
            return;
        std::vector<int> success(n);
        // The return value only says whether every point succeeded
        ct->Transform(n, x, y, nullptr, success.data());
        for (size_t i = 0; i < n; i++)
        {
            if (!success[i])
                x[i] = y[i] = HUGE_VAL;
        }
    }

}
