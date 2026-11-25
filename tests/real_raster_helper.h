/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef REAL_RASTER_HELPER_H
#define REAL_RASTER_HELPER_H

#include "json.h"
#include "raster_helper.h"

namespace testutils
{
    // The raster helper of the program (gdal-util), for a test that counts or changes one of its
    // operations: it overrides that one and calls the base for the work
    class RealRasterHelper : public ddb::raster::IRasterHelper
    {
      public:
        ddb::raster::ProbeResult probe(const fs::path &path, const ddb::raster::ProbeParams &params) override
        {
            return real_.probe(path, params);
        }
        ddb::PointCloudInfo pointCloudInfo(const fs::path &path) override { return real_.pointCloudInfo(path); }
        void thumbnail(const fs::path &path, const fs::path &out, const ddb::raster::ThumbnailParams &params) override
        {
            real_.thumbnail(path, out, params);
        }
        void photoTile(const fs::path &path, const fs::path &out, const ddb::raster::PhotoTileParams &params) override
        {
            real_.photoTile(path, out, params);
        }
        void normalize(const fs::path &path, const fs::path &out, const json &params) override
        {
            real_.normalize(path, out, params);
        }
        void geoproject(const fs::path &path, const fs::path &out, const json &params) override
        {
            real_.geoproject(path, out, params);
        }
        void buildCog(const fs::path &path, const fs::path &out, const json &params,
                      const ddb::raster::ProgressCallback &progress) override
        {
            real_.buildCog(path, out, params, progress);
        }
        json buildCopc(const fs::path &path, const fs::path &out, const json &params,
                       const ddb::raster::ProgressCallback &progress) override
        {
            return real_.buildCopc(path, out, params, progress);
        }
        void buildVector(const fs::path &path, const fs::path &out, const json &params,
                         const ddb::raster::ProgressCallback &progress) override
        {
            real_.buildVector(path, out, params, progress);
        }
        void sampleCloud(const fs::path &path, const fs::path &out, const json &params,
                         const ddb::raster::ProgressCallback &progress) override
        {
            real_.sampleCloud(path, out, params, progress);
        }
        json operate(const char *op, const fs::path &path, const fs::path &out, const json &params,
                     const ddb::raster::ProgressCallback &progress) override
        {
            return real_.operate(op, path, out, params, progress);
        }

      private:
        ddb::raster::GdalUtilHelper real_;
    };
} // namespace testutils

#endif // REAL_RASTER_HELPER_H
