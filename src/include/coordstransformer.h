/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef COORDSTRANSFORMER_H
#define COORDSTRANSFORMER_H

#include <cstddef>
#include <memory>
#include <string>

#include "gdal_inc.h"
#include "ddb_export.h"

namespace ddb
{

    // Transforms points between two coordinate reference systems.
    //
    // Every transformer sets the traditional GIS axis order on its two ends itself: x is the
    // easting or longitude and y the northing or latitude whatever the authority axis order of
    // the CRS (EPSG:4326 and EPSG:4258 list latitude first, EPSG:3035 northing first). Nothing
    // depends on the process-wide OGR_CT_FORCE_TRADITIONAL_GIS_ORDER setting, and the SRS
    // objects passed in are copied, never changed.
    //
    // Owns its SRS pair and transform by value / unique_ptr, so a constructor that throws
    // leaks nothing and copies cannot double-free. Movable, not copyable.
    class CoordsTransformer
    {
        OGRSpatialReference src;
        OGRSpatialReference tgt;
        std::unique_ptr<OGRCoordinateTransformation> ct;

        struct NoThrow
        {
        };
        CoordsTransformer(const OGRSpatialReference &from, const OGRSpatialReference &to, NoThrow);

        void forceTraditionalAxisOrder();

    public:
        // Throws GDALException when GDAL cannot build the transformation
        DDB_DLL CoordsTransformer(const OGRSpatialReference &from, const OGRSpatialReference &to);
        DDB_DLL CoordsTransformer(int epsgFrom, int epsgTo);
        DDB_DLL CoordsTransformer(const std::string &wktFrom, int epsgTo);
        DDB_DLL CoordsTransformer(int epsgFrom, const std::string &wktTo);
        DDB_DLL ~CoordsTransformer();

        CoordsTransformer(const CoordsTransformer &) = delete;
        CoordsTransformer &operator=(const CoordsTransformer &) = delete;
        DDB_DLL CoordsTransformer(CoordsTransformer &&) noexcept;
        DDB_DLL CoordsTransformer &operator=(CoordsTransformer &&) noexcept;

        // Null when GDAL cannot build the transformation (no operation between the two CRSs):
        // for callers that report that failure in their own words or go on without it
        DDB_DLL static std::unique_ptr<CoordsTransformer> tryCreate(const OGRSpatialReference &from,
                                                                    const OGRSpatialReference &to);

        // These throw GDALException when the point cannot be transformed
        DDB_DLL void transform(double *x, double *y);
        DDB_DLL void transform(double *x, double *y, double *z);

        // Transforms n points in place in one PROJ call. Points that cannot be
        // transformed are set to HUGE_VAL instead of failing the call.
        DDB_DLL void transform(size_t n, double *x, double *y);

        // Transforms n points in place (z may be null); false when any point failed
        DDB_DLL bool tryTransform(size_t n, double *x, double *y, double *z = nullptr);

        // The transformation, for GDAL APIs that take one (OGR_G_Transform, OGRGeometry::transform)
        OGRCoordinateTransformation *get() const noexcept { return ct.get(); }
    };

}

#endif // COORDSTRANSFORMER_H
