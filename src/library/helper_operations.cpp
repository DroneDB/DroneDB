/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "helper_operations.h"

#include <string>

#include "exceptions.h"
#include "mask.h"
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
