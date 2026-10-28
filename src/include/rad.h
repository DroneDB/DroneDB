/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef RAD_H
#define RAD_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "fs.h"

namespace ddb
{
    // Minimal reader for the Spark "RAD" Gaussian Splat level-of-detail container
    // (produced by build-lod). The format stores splats coarse-to-fine, chunked at
    // 65536 splats; the first chunk(s) are a low-density view of the whole scene, so
    // they can be decoded on their own to render a thumbnail - the direct analog of
    // reading only the coarse octree levels of a COPC point cloud.
    //
    // Only the attributes needed for previews, bounds and the orientation of the scene are
    // decoded (center, rgb, alpha, scales, orientation); spherical harmonics are skipped.

    // A coarse splat set decoded from one or more RAD chunks. Colours are already in
    // display space (RGB in [0,1]) and opacities are activated (NOT logits), matching
    // the RAD encoding - so consumers must not re-apply SH or sigmoid transforms.
    struct RadCoarseSplats
    {
        size_t count = 0;
        std::vector<float> positions; // count * 3, in the RAD's local coordinate space
        std::vector<float> colors;    // count * 3, display RGB in [0,1]
        std::vector<float> opacities; // count, activated opacity
        // The shape of each splat: the standard deviations along its own axes (linear, not the
        // logarithm RAD stores; 0 for a splat of no extent) and the rotation of those axes as a unit
        // quaternion x, y, z, w. A chunk without them yields scale 0 and the identity.
        std::vector<float> scales;    // count * 3
        std::vector<float> rotations; // count * 4
    };

    // The splats of a chunk, as build-lod writes them (the last chunk of a file may hold fewer)
    constexpr size_t RadChunkSplats = 65536;

    // The chunks the up vector of a scene is estimated from (readRadCoarseSplats with onlyTheCut), at
    // build time for bounds.json and by the thumbnail, so both stand the scene the same way. The first
    // chunk is the coarsest picture of the scene; at 4 times that its ground is clear. A thumbnail
    // reads at least these, and more for a larger picture.
    constexpr int RadPreviewChunks = 4;

    // True when the path ends with the canonical .rad delivery extension.
    bool isRadPath(const std::string &filename);

    // Decodes up to `maxChunks` leading chunks (coarsest first) of a single-file RAD
    // into display-space splats suitable for rendering a preview. Throws AppException
    // on a malformed or unreadable file.
    //
    // The splats of a level-of-detail tree overlap: each is the merged image of the finer splats that
    // refine it. With `onlyTheCut` the splats refined by others in the chunks read are left out, so
    // what comes back is the finest picture of the scene those chunks hold, each region once. (A
    // file without a tree has no splat refined by another.)
    RadCoarseSplats readRadCoarseSplats(const fs::path &radPath, int maxChunks = 1, bool onlyTheCut = false);

    // Computes the axis-aligned bounding box of every splat centre in the RAD by
    // streaming each chunk's `center` property (bounded memory, exact, in the RAD's
    // coordinate space). Returns false when no centres could be decoded.
    bool computeRadBounds(const fs::path &radPath,
                          std::array<double, 3> &outMin,
                          std::array<double, 3> &outMax);

} // namespace ddb

#endif // RAD_H
