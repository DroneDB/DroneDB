/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "helper_operations.h"

#include <numeric>
#include <string>
#include <vector>

#include "exceptions.h"
#include "align.h"
#include "mask.h"
#include "merge_multispectral.h"
#include "photo_resize.h"
#include "raster_ops.h"
#include "raster_stats.h"
#include "tilers_raster.h"
#include "gdal_open_policy.h"
#include "logger.h"
#include "mio.h"

namespace ddb::raster
{
    namespace
    {
        // The exact statistics of every band of the user's raster `path`: read by ddb-tilers when it reads
        // the file (a GeoTIFF: the reader of the COG build, so they are the statistics the build counts),
        // else by GDAL, with the same rules (computeExactStatistics)
        json statistics(const fs::path &path, const StatisticsParams &params, const ProgressCallback &progress)
        {
            const auto compute = [&](const RasterSource &source) {
                std::vector<int> bands(static_cast<size_t>(source.bandCount()));
                std::iota(bands.begin(), bands.end(), 1);
                const auto stats = computeExactStatistics(source, bands, params.nodata,
                                                          static_cast<size_t>(params.threads.value_or(0)), progress);
                StatisticsByBand byBand;
                for (size_t i = 0; i < bands.size(); i++)
                    if (stats[i])
                        byBand[bands[i]] = *stats[i];
                return json{{"bands", statisticsByBandToJson(byBand)}};
            };

            std::unique_ptr<tilers::Raster> tilersRaster;
            try
            {
                tilersRaster = tilers::Raster::openPrivate(path);
            }
            catch (const std::exception &e)
            {
                LOGD << "ddb-tilers does not read " << io::toUtf8(path) << ", GDAL does: " << e.what();
            }
            if (tilersRaster)
                return compute(*rasterSourceOf(*tilersRaster));

            const auto dataset = openUserRaster(io::toUtf8(path));
            if (!dataset)
                throw GDALException("Cannot open " + io::toUtf8(path));
            return compute(*rasterSourceOf(dataset.get()));
        }

        json run(std::string_view op, const fs::path &path, const fs::path &out, const json &params,
                 const ProgressCallback &progress)
        {
            if (op == op::kMask)
            {
                maskRasterFile(path, out, maskParamsFromJson(params), progress);
                return json::object();
            }
            if (op == op::kRasterShapes)
            {
                const RasterShapesParams shapes = rasterShapesParamsFromJson(params);
                return toJson(readRasterShapes(shapes.inputs, shapes.xmp));
            }
            if (op == op::kMergeBands)
                return mergeBandsFile(mergeBandsParamsFromJson(params), out);
            if (op == op::kMergePreview)
            {
                mergePreviewFile(mergePreviewParamsFromJson(params), out);
                return json::object();
            }
            if (op == op::kAlign)
                return alignOperation(path, out, params);
            if (op == op::kAlignValidation)
                return alignValidationOperation(path, params);
            if (op == op::kResizeImages)
                return resizeResultsToJson(resizePhotos(resizeImagesParamsFromJson(params), out, progress));
            if (op == op::kStatistics)
                return statistics(path, statisticsParamsFromJson(params), progress);
            throw ProtocolError("'" + std::string(op) + "' is not an operation of the helper");
        }
    } // namespace

    json runHelperOperation(std::string_view op, const fs::path &path, const fs::path &out, const json &params,
                            const ProgressCallback &progress)
    {
        try
        {
            return run(op, path, out, params, progress);
        }
        catch (const InvalidArgsException &e)
        {
            // The operation refuses its arguments for this file (a mask of a raster of 2 bands): the
            // host raises it again as such, as the call did when it ran the operation itself
            throw RasterOpException(ErrorCode::Invalid, e.what());
        }
    }
} // namespace ddb::raster
