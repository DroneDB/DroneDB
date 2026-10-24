/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DQ06_GOLDEN_SUPPORT_H
#define DQ06_GOLDEN_SUPPORT_H

// Golden outputs of the geo and raster code (DQ-06). The refactor of that code is meant to be
// output-neutral, so the outputs of the old code are stored in tests/dq06_golden/<name>.json
// and every run compares with them.
//
// Numbers are compared with a relative tolerance of 1e-9: the goldens must also hold on
// platforms whose libm differs in the last bit. Two environment variables serve the exact
// comparison between two builds:
//   DDB_DQ06_DUMP_DIR=<dir>      writes every actual output, unrounded, to <dir>/<name>.json
//   DDB_UPDATE_DQ06_GOLDEN=1     rewrites the goldens from the actual outputs

#include <functional>
#include <string>

#include "fs.h"
#include "json.h"

namespace dq06
{
    // Compares actual with the golden `name` (a failure when it differs), after dropping the
    // volatile keys (timestamps)
    void expectGolden(const std::string &name, json actual);

    // `message` with `folder` (a folder of the test) replaced by "<dir>" and the rest of that
    // path in '/' separators, so that the goldens are the same on Windows
    std::string scrubFolder(std::string message, const std::string &folder);

    // The JSON a call returns; an exception becomes {"error": what()} so that error messages
    // are part of the golden. `scrub` (a folder of the test) is replaced in them (scrubFolder).
    json capture(const std::function<std::string()> &call, const std::string &scrub = "");

    // Writes `value` to DDB_DQ06_DUMP_DIR/<name>.json when that variable is set, and nothing
    // else: the exact hashes of outputs whose bytes are not portable
    void dumpOnly(const std::string &name, const json &value);

    // Numbers equal to the relative tolerance 1e-9 (absolute 1e-9 near zero), everything else
    // exactly. On a difference `where` names the JSON pointer.
    bool jsonNear(const json &a, const json &b, std::string &where, const std::string &at = "");

    // 64-bit FNV-1a of a byte range, as 16 hex digits
    std::string fnv1a(const void *data, size_t size);
}

#endif // DQ06_GOLDEN_SUPPORT_H
