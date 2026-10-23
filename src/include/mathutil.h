/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef MATHUTIL_H
#define MATHUTIL_H

#include <cmath>
#include <numbers>
#include <type_traits>

namespace ddb {

// The one definition of pi. M_PI is not standard C++ (MSVC hides it behind _USE_MATH_DEFINES),
// so it must not be used or redefined.
inline constexpr double kPi = std::numbers::pi;

// How close two floats must be to count as the same value (sameFloat)
inline constexpr double kFloatEpsilon = 0.000001;

// True when value is a positive integral power of two (1, 2, 4, ...).
template <typename T>
constexpr bool isPowerOfTwo(T value) {
    static_assert(std::is_integral_v<T>, "isPowerOfTwo needs an integral type");
    return value > 0 && (value & (value - 1)) == 0;
}

// The conversion factors. `x * kDegToRad` is not `deg2rad(x)`: that one is `(x * kPi) / 180.0`,
// which rounds differently in the last bit for some x. Code that always multiplied by the factor
// keeps doing so (its results are pinned by the golden outputs of DQ-06).
inline constexpr double kDegToRad = kPi / 180.0;
inline constexpr double kRadToDeg = 180.0 / kPi;

constexpr double rad2deg(double rad) noexcept {
    return (rad * 180.0) / kPi;
}

constexpr double deg2rad(double deg) noexcept {
    return (deg * kPi) / 180.0;
}

// True when a and b differ by less than kFloatEpsilon
inline bool sameFloat(float a, float b) noexcept {
    return std::fabs(a - b) < kFloatEpsilon;
}

}  // namespace ddb

#endif  // MATHUTIL_H
