/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "helper_operations.h"

#include <algorithm>
#include <array>
#include <numeric>
#include <string>
#include <vector>

#include "exceptions.h"
#include "align.h"
#include "mask.h"
#include "parallel.h"
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

        // The windows that cover a level of `width` x `height` pixels: whole blocks of `blockWidth` x
        // `blockHeight`, as many as fit in a window of at most kValidateWindowPixels, so a strip TIFF is
        // read a few strips at a time and a tiled one a row of tiles at a time
        constexpr size_t kValidateWindowPixels = 4 * 1024 * 1024;

        std::vector<std::array<int, 4>> levelWindows(int width, int height, int blockWidth, int blockHeight)
        {
            const int bw = std::clamp(blockWidth, 1, width), bh = std::clamp(blockHeight, 1, height);
            const size_t perRow = static_cast<size_t>(width) * static_cast<size_t>(bh);
            // Whole rows of blocks when they fit, else blocks of one row at a time
            const int rows = static_cast<int>(std::max<size_t>(1, kValidateWindowPixels / std::max<size_t>(perRow, 1)));
            const int columns = perRow <= kValidateWindowPixels
                                    ? width
                                    : std::max(bw, static_cast<int>(kValidateWindowPixels / static_cast<size_t>(bh)) / bw * bw);
            std::vector<std::array<int, 4>> windows;
            for (int y = 0; y < height; y += rows * bh)
                for (int x = 0; x < width; x += columns)
                    windows.push_back({x, y, std::min(columns, width - x), std::min(rows * bh, height - y)});
            return windows;
        }

        // Whether ddb-tilers reads the user's raster `path` whole, which lets the host read it itself
        // (raster_input.h isHostReadable): it opens it here, in the confined helper, and decodes every
        // block of every level with the masks: the full resolution in the exact statistics pass (the
        // statistics are the result, as the `statistics` operation gives them), then each overview a
        // window at a time on the threads of the call. A file ddb-tilers does not open (not a TIFF) or
        // fails to decode is not host readable, with the reason; nothing else is an error.
        json validate(const fs::path &path, const ValidateParams &params, const ProgressCallback &progress)
        {
            const auto notReadable = [](const std::string &reason) {
                return json{{"hostReadable", false}, {"reason", reason.substr(0, 2000)}};
            };
            std::unique_ptr<tilers::Raster> raster;
            try
            {
                raster = tilers::Raster::openPrivate(path);
            }
            catch (const AppException &e)
            {
                return notReadable(std::string("ddb-tilers does not open it: ") + e.what());
            }

            const size_t threads = static_cast<size_t>(params.threads.value_or(0));
            std::vector<int> bands(static_cast<size_t>(raster->bandCount()));
            std::iota(bands.begin(), bands.end(), 1);
            // The share of the pixels in the full resolution, for the progress of the two passes
            double levelPixels = static_cast<double>(raster->width()) * raster->height(), overviewPixels = 0;
            for (const auto &[w, h] : raster->overviews())
                overviewPixels += static_cast<double>(w) * h;
            const double fullShare = levelPixels / (levelPixels + overviewPixels);
            const auto report = [&](double fraction) {
                if (progress)
                    progress(fraction);
            };

            try
            {
                const auto stats = computeExactStatistics(*rasterSourceOf(*raster), bands, std::nullopt, threads,
                                                          [&](double f) { report(f * fullShare); });
                StatisticsByBand byBand;
                for (size_t i = 0; i < bands.size(); i++)
                    if (stats[i])
                        byBand[bands[i]] = *stats[i];

                double done = 0;
                for (size_t level = 1; level <= raster->overviews().size(); level++)
                {
                    const auto [width, height] = raster->overviews()[level - 1];
                    const auto windows = levelWindows(width, height, raster->blockWidth(), raster->blockHeight());
                    // A batch of windows on the threads, then the progress on this thread
                    const size_t batch = std::max<size_t>(1, threads) * 4;
                    for (size_t first = 0; first < windows.size(); first += batch)
                    {
                        const size_t count = std::min(batch, windows.size() - first);
                        parallel::forEachIndex(count, threads, [&](size_t i) {
                            const auto [x, y, w, h] = windows[first + i];
                            const size_t pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
                            std::vector<double> values(pixels * bands.size());
                            std::vector<uint8_t> mask(pixels, 255);
                            raster->readLevel(static_cast<int>(level), x, y, w, h, bands, values.data(),
                                              mask.data());
                        });
                        for (size_t i = first; i < first + count; i++)
                            done += static_cast<double>(windows[i][2]) * windows[i][3];
                        report(fullShare + (1.0 - fullShare) * done / std::max(overviewPixels, 1.0));
                    }
                }
                return json{{"hostReadable", true}, {"bands", statisticsByBandToJson(byBand)}};
            }
            catch (const CanceledException &)
            {
                throw;
            }
            catch (const AppException &e)
            {
                return notReadable(std::string("ddb-tilers cannot decode it: ") + e.what());
            }
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
            if (op == op::kValidate)
                return validate(path, validateParamsFromJson(params), progress);
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
