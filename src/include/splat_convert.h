/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef SPLAT_CONVERT_H
#define SPLAT_CONVERT_H

#include <cstdint>
#include <string>

#include "fs.h"

namespace ddb
{
    // Converts a splat source (any format SplatStage::Convert names: .ply | .splat | .spz) into a
    // single .spz file. The output is written whole or not at all (a temporary file renamed onto
    // it), and it must not be the input (InvalidArgsException). A format that is not converted
    // (.ksplat, .sog) is an InvalidArgsException; any other failure is an AppException.
    //
    // The spherical-harmonics quantization follows the environment variable DDB_GSPLAT_SH_BITS
    // ("5" sets the first band's bits, "5,4" also the others', each in [1,8]; a malformed value
    // is ignored).
    void convertToSpz(const std::string &input, const std::string &output);

    // convertToSpz() refuses a format that is not converted: this is the same refusal, for a caller
    // that has to know before it takes a lock or reads anything (InvalidArgsException).
    void assureConvertibleToSpz(const std::string &input);

    // Rewrites an SPZ of any version as the gzip-based version 3 that the viewer and build-lod
    // read (a v4 NGSP/ZSTD file is not readable by either). The quaternion precision is the same,
    // so the splats do not change. Throws AppException when nothing can be decoded from the input.
    void transcodeSpzToV3(const fs::path &input, const fs::path &output);

    // The splat count as the int32_t SPZ keeps it. A count SPZ cannot hold is an AppException that
    // says so, instead of a wrapped count and a file that is silently cut short.
    int32_t spzPointCount(uint64_t splats, const std::string &source);

} // namespace ddb

#endif // SPLAT_CONVERT_H
