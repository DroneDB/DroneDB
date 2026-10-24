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

    void CoordsTransformer::forceTraditionalAxisOrder()
    {
        src.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        tgt.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    }

    CoordsTransformer::CoordsTransformer(const OGRSpatialReference &from, const OGRSpatialReference &to, NoThrow)
        : src(from), tgt(to)
    {
        forceTraditionalAxisOrder();
        ct.reset(OGRCreateCoordinateTransformation(&src, &tgt));
    }

    CoordsTransformer::CoordsTransformer(const OGRSpatialReference &from, const OGRSpatialReference &to)
        : CoordsTransformer(from, to, NoThrow{})
    {
        if (!ct)
            throw GDALException("Failed to create coordinate transformation");
    }

    std::unique_ptr<CoordsTransformer> CoordsTransformer::tryCreate(const OGRSpatialReference &from,
                                                                    const OGRSpatialReference &to)
    {
        std::unique_ptr<CoordsTransformer> t(new CoordsTransformer(from, to, NoThrow{}));
        if (!t->ct)
            return nullptr;
        return t;
    }

    namespace
    {
        OGRSpatialReference srsOfEpsg(int epsg)
        {
            OGRSpatialReference srs;
            importEpsg(srs, epsg);
            return srs;
        }

        OGRSpatialReference srsOfWkt(const std::string &wkt)
        {
            OGRSpatialReference srs;
            importWkt(srs, wkt);
            return srs;
        }
    }

    CoordsTransformer::CoordsTransformer(const std::string &wktFrom, int epsgTo)
        : CoordsTransformer(srsOfWkt(wktFrom), srsOfEpsg(epsgTo))
    {
    }

    CoordsTransformer::CoordsTransformer(int epsgFrom, const std::string &wktTo)
        : CoordsTransformer(srsOfEpsg(epsgFrom), srsOfWkt(wktTo))
    {
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

    bool CoordsTransformer::tryTransform(size_t n, double *x, double *y, double *z)
    {
        if (n == 0)
            return true;
        return ct->Transform(n, x, y, z) != 0;
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
