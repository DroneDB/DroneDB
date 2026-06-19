/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef RAD_H
#define RAD_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "ddb_export.h"
#include "fs.h"

namespace ddb
{
    // Minimal reader for the Spark "RAD" Gaussian Splat level-of-detail container
    // (produced by build-lod). The format stores splats coarse-to-fine, chunked at
    // 65536 splats; the first chunk(s) are a low-density view of the whole scene, so
    // they can be decoded on their own to render a thumbnail - the direct analog of
    // reading only the coarse octree levels of a COPC point cloud.
    //
    // Only the attributes needed for previews and bounds are decoded (center, rgb,
    // alpha); scales / orientation / spherical harmonics are skipped.

    // A coarse splat set decoded from one or more RAD chunks. Colours are already in
    // display space (RGB in [0,1]) and opacities are activated (NOT logits), matching
    // the RAD encoding - so consumers must not re-apply SH or sigmoid transforms.
    struct RadCoarseSplats
    {
        size_t count = 0;
        std::vector<float> positions; // count * 3, in the RAD's local coordinate space
        std::vector<float> colors;    // count * 3, display RGB in [0,1]
        std::vector<float> opacities; // count, activated opacity (clamped to >= 0)
    };

    // True when the path ends with the canonical .rad delivery extension.
    DDB_DLL bool isRadPath(const std::string &filename);

    // Decodes up to `maxChunks` leading chunks (coarsest first) of a single-file RAD
    // into display-space splats suitable for rendering a preview. Throws AppException
    // on a malformed or unreadable file.
    DDB_DLL RadCoarseSplats readRadCoarseSplats(const fs::path &radPath, int maxChunks = 1);

    // Computes the axis-aligned bounding box of every splat centre in the RAD by
    // streaming each chunk's `center` property (bounded memory, exact, in the RAD's
    // coordinate space). Returns false when no centres could be decoded.
    DDB_DLL bool computeRadBounds(const fs::path &radPath,
                                  std::array<double, 3> &outMin,
                                  std::array<double, 3> &outMax);

} // namespace ddb

#endif // RAD_H
