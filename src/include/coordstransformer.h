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

    // Owns its SRS pair and transform by value / unique_ptr, so a constructor that throws
    // leaks nothing and copies cannot double-free. Movable, not copyable.
    class CoordsTransformer
    {
        OGRSpatialReference src;
        OGRSpatialReference tgt;
        std::unique_ptr<OGRCoordinateTransformation> ct;

        void createTransform();

    public:
        DDB_DLL CoordsTransformer(int epsgFrom, int epsgTo);
        DDB_DLL CoordsTransformer(const std::string &wktFrom, int epsgTo);
        DDB_DLL CoordsTransformer(int epsgFrom, const std::string &wktTo);
        DDB_DLL ~CoordsTransformer();

        CoordsTransformer(const CoordsTransformer &) = delete;
        CoordsTransformer &operator=(const CoordsTransformer &) = delete;
        DDB_DLL CoordsTransformer(CoordsTransformer &&) noexcept;
        DDB_DLL CoordsTransformer &operator=(CoordsTransformer &&) noexcept;

        DDB_DLL void transform(double *x, double *y);
        DDB_DLL void transform(double *x, double *y, double *z);

        // Transforms n points in place in one PROJ call. Points that cannot be
        // transformed are set to HUGE_VAL instead of failing the call.
        DDB_DLL void transform(size_t n, double *x, double *y);
    };

}

#endif // COORDSTRANSFORMER_H
