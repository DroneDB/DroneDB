/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef HELPER_OPERATIONS_H
#define HELPER_OPERATIONS_H

#include <string_view>

#include "fs.h"
#include "json.h"
#include "raster_protocol.h"

namespace ddb::raster
{
    // Runs the operation `op` (isHelperOperation) on the user's raster `path`, writing `out`, with
    // the params the protocol defines for it: what the raster helper runs for such a request, and
    // the one place that maps an operation to its code. Runs where users' files are opened: the
    // helper (or a test that plays it). Returns the result of the operation.
    // @throws ProtocolError for an operation that is none or params that are not its own; the
    // exceptions of the operation
    json runHelperOperation(std::string_view op, const fs::path &path, const fs::path &out, const json &params,
                            const ProgressCallback &progress);
} // namespace ddb::raster

#endif // HELPER_OPERATIONS_H
