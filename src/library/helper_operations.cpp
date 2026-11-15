/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "helper_operations.h"

#include <string>

#include "exceptions.h"
#include "align.h"
#include "mask.h"
#include "merge_multispectral.h"
#include "photo_resize.h"
#include "raster_ops.h"

namespace ddb::raster
{
    namespace
    {
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
