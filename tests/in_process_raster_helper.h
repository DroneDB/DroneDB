/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef IN_PROCESS_RASTER_HELPER_H
#define IN_PROCESS_RASTER_HELPER_H

#include <stdexcept>

#include "json.h"

#include "raster_helper.h"
#include "raster_ops.h"

namespace testutils
{
    // A raster helper that runs the operations of gdal-util in the process of the test, for the
    // tests that look into the process while an operation runs (its GDAL log, a write of a GDAL
    // driver that the test makes fail, the cache of GDAL). The tests that only need the result use
    // the program. Install it with ddb::raster::ScopedRasterHelper.
    class InProcessRasterHelper : public ddb::raster::IRasterHelper
    {
    public:
        ddb::raster::ProbeResult probe(const fs::path &path, const ddb::raster::ProbeParams &params) override
        {
            return ddb::raster::probe(path, params);
        }
        void thumbnail(const fs::path &path, const fs::path &out, const ddb::raster::ThumbnailParams &params) override
        {
            ddb::raster::thumbnail(path, out, params);
        }
        void normalize(const fs::path &, const fs::path &, const json &) override
        {
            throw std::logic_error("normalize is not available in this version");
        }
        void geoproject(const fs::path &path, const fs::path &out, const json &params) override
        {
            ddb::raster::geoprojectImage(path, out, ddb::raster::geoprojectParamsFromJson(params));
        }
        void buildCog(const fs::path &path, const fs::path &out, const json &params,
                      const ddb::raster::ProgressCallback &progress) override
        {
            ddb::raster::buildCogFile(path, out, ddb::raster::cogParamsFromJson(params), progress);
        }
        void buildVector(const fs::path &path, const fs::path &out, const json &params,
                         const ddb::raster::ProgressCallback &) override
        {
            ddb::raster::buildVectorFiles(path, out, ddb::raster::vectorParamsFromJson(params));
        }
    };
}

#endif // IN_PROCESS_RASTER_HELPER_H
