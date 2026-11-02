/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef SPLAT_FRAME_H
#define SPLAT_FRAME_H

#include <array>
#include <vector>

#include "json_fwd.h"
#include "rad.h"

namespace ddb
{
    // Which way is up in a Gaussian splat. Trainers give a splat the frame of their cameras: COLMAP
    // and INRIA scenes are Y-down, ODM scenes Z-up, and nothing in the file says which. The estimate
    // is made from the coarse splats of the built RAD, written to bounds.json, and used by the
    // viewers and by the thumbnail to stand the scene upright.

    // How the up vector was found; the names are the `upSource` values of bounds.json.
    enum class SplatUpSource
    {
        Georef,      // the splat is georeferenced: up is +Z, exactly
        GroundPlane, // the dominant plane of the splats, the side most of the others are on
        Pca          // the flattest axis of all the splats
    };
    const char *splatUpSourceName(SplatUpSource source);

    // The viewer stands a scene up by its estimate when upConfidence is at least this much.
    constexpr double SplatUpConfident = 0.6;

    struct SplatUp
    {
        std::array<double, 3> up{0.0, 0.0, 1.0}; // unit vector, in the frame of the splats, to the sky
        double confidence = 0.0;                 // [0, 1]
        SplatUpSource source = SplatUpSource::Pca;
    };

    /// {"up": [x, y, z], "upConfidence", "upSource"}: the members bounds.json adds to its bounding box
    void to_json(json &j, const SplatUp &up);

    // Estimates the up vector of a scene from the centres of its splats (`positions`, 3 floats each)
    // weighted by their activated `opacities` (one each; an empty array weighs all as 1).
    //
    // A georeferenced scene is in a local ENU or projected frame, so up is +Z with confidence 1.
    // Any other is estimated, deterministically (the same splats give the same answer). A confident
    // answer is a claim the viewers stand the scene on, so the confidence is kept low wherever the
    // splats allow two answers, rather than right on average:
    //
    //  1. The ground is the dominant plane. RANSAC finds six planes one after the other, each among the
    //     splats the earlier ones do not hold, scored by the opacity within 2% of the extent of the scene
    //     (the 1st to 99th percentile of each axis) and refitted to their own splats. The one kept holds
    //     the most opacity of its own weighted by |cos|^4 of its angle to the flattest axis of all the
    //     splats (PCA): in a room a wall can hold more splats than the floor, and a room is lower than it
    //     is wide. The search is made again with planes a quarter as thick, and that plane gives `up`.
    //  2. The side is the one the scene stands on: the densest layer along the normal is the ground, and
    //     the opacity off it (sideBias = (above - below) / (above + below)) and the median height
    //     (medianOffset, in standard deviations, 0 within the layer) are both above it. When they point
    //     to opposite sides, the side is unknown.
    //     confidence = ramp(support, 0.08, 0.30) * ramp(spread, 0.05, 0.25) * ramp(flatness, 0.3, 0.6)
    //                  * max(ramp(sideBias, 0.1, 0.5), ramp(medianOffset, 0.05, 0.25))
    //                  * (1 - ramp(tilt, 8, 15))
    //     where support is the share of all the opacity on the thick plane, spread is sqrt(l2 / l1) of
    //     its own two spreads (a line of splats is not a ground), flatness is 1 - sqrt(l3 / l2) of the
    //     three spreads l1 >= l2 >= l3 of all the splats (a room as high as it is wide has no axis to
    //     tell its floor from its walls), tilt is the angle in degrees between the thick and the thin
    //     plane (a wooded slope that is the densest plane only in the thick slab, beside a subject on
    //     level ground, leaves two grounds), and ramp(x, a, b) goes from 0 at a to 1 at b.
    //  3. When the thick plane holds less than 12% of the opacity there is no ground to speak of, and
    //     the flattest axis of all the splats is used instead. Its sign is that of the skew of the
    //     splats along it: a floor with things on it has a long tail upward.
    //     confidence = 0.8 * ramp(flatness, 0.3, 0.8) * ramp(|skew|, 0.2, 1.0)
    //     A fit of everything is a weaker claim than a fitted ground, hence the 0.8 ceiling.
    // A ball of splats, or too few of them, has no flat axis and comes out at confidence 0.
    SplatUp estimateSplatUp(const std::vector<float> &positions, const std::vector<float> &opacities,
                            bool georeferenced = false);
    SplatUp estimateSplatUp(const RadCoarseSplats &splats, bool georeferenced = false);

    // The principal axes of points weighted by `weights` (one each, none negative): their weighted mean,
    // the variances along the axes (the widest first) and the axes themselves, unit vectors in the same
    // order. The PCA estimateSplatUp falls back on; a thumbnail uses it to tell a room from the open air.
    struct SplatAxes
    {
        std::array<double, 3> mean{};
        std::array<double, 3> variances{};
        std::array<std::array<double, 3>, 3> axes{};
    };
    SplatAxes principalAxes(const std::vector<std::array<double, 3>> &points, const std::vector<double> &weights);

} // namespace ddb

#endif // SPLAT_FRAME_H
