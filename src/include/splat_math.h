/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef SPLAT_MATH_H
#define SPLAT_MATH_H

#include <algorithm>
#include <cmath>

// SPZ (vendored, MIT): invSigmoid is the one logit the project has.
#include "splat-utils.h"

namespace ddb::splat_math
{
    // Standard spherical-harmonics DC -> RGB factor (Y_0^0 normalization constant).
    constexpr double ShC0 = 0.2820947917738781;

    // Activated opacity of a stored logit.
    inline float sigmoid(float logit)
    {
        return 1.0f / (1.0f + std::exp(-logit));
    }

    // The logit of an opacity in [0, 1] as SPZ stores it. The opacity is kept inside (0, 1): a
    // fully transparent or opaque splat (the 0 and 255 of an 8-bit alpha) has no finite logit.
    inline float logit(float opacity)
    {
        constexpr float eps = 1e-6f;
        return spz::invSigmoid(std::min(std::max(opacity, eps), 1.0f - eps));
    }

    // Display RGB of the SH DC coefficient (the colour a splat has seen face-on).
    inline float dcToColor(float dc)
    {
        return static_cast<float>(0.5 + ShC0 * static_cast<double>(dc));
    }

    // The SH DC coefficient of a display RGB value in [0, 1].
    inline float colorToDc(float color)
    {
        return (color - 0.5f) / static_cast<float>(ShC0);
    }
} // namespace ddb::splat_math

#endif // SPLAT_MATH_H
